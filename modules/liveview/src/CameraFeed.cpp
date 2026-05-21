#include "CameraFeed.h"

#include <chrono>
#include <iostream>
#include <thread>

CameraFeed::CameraFeed(const std::string& grpc_addr, int32_t camera_id)
    : cam_(grpc_addr), camera_id_(camera_id) {}

CameraFeed::~CameraFeed() {
    Stop();
}

void CameraFeed::Start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&CameraFeed::GrabLoop, this);
}

void CameraFeed::Stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

bool CameraFeed::IsNewFrame() {
    return frame_new_.exchange(false, std::memory_order_acq_rel);
}

std::optional<GigECamera::PinnedSlot> CameraFeed::TryPinLatest() {
    return cam_.pin_latest(camera_id_);
}

void CameraFeed::ReleasePin(int32_t slot_idx) {
    cam_.release_pin(slot_idx);
}

bool CameraFeed::GrabInto(GigeFrame& out) {
    return cam_.grab_direct_into(camera_id_, out);
}

float CameraFeed::GetFps() const {
    std::lock_guard<std::mutex> lk(fps_mutex_);
    return fps_;
}

std::optional<GigeCameraInfo> CameraFeed::QueryInfo() {
    return cam_.info(camera_id_);
}

void CameraFeed::GrabLoop() {
    using clk = std::chrono::steady_clock;
    int64_t last_ts = -1;

    while (running_.load(std::memory_order_acquire)) {
        // Cheap SHM-only timestamp check — no gRPC, no pixel copy.
        int64_t shm_ts = -1;
        try {
            shm_ts = cam_.latest_timestamp_us(camera_id_);
        } catch (const std::exception& e) {
            std::cerr << "[CameraFeed] SHM error (cam " << camera_id_
                      << "): " << e.what() << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        if (shm_ts <= 0 || shm_ts == last_ts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        last_ts = shm_ts;
        frame_new_.store(true, std::memory_order_release);

        const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            clk::now().time_since_epoch()).count();

        {
            std::lock_guard<std::mutex> lk(fps_mutex_);
            frame_times_us_.push_back(now_us);
            if (static_cast<int>(frame_times_us_.size()) > kFpsWindow)
                frame_times_us_.pop_front();
            if (frame_times_us_.size() >= 2) {
                const double span_s =
                    (frame_times_us_.back() - frame_times_us_.front()) * 1e-6;
                fps_ = static_cast<float>((frame_times_us_.size() - 1) / span_s);
            }
        }
    }
}
