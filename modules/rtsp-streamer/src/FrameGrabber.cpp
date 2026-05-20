#include "FrameGrabber.h"

#include <chrono>
#include <iostream>
#include <thread>

FrameGrabber::FrameGrabber(const std::string& grpc_addr, int32_t camera_id)
    : cam_(grpc_addr), camera_id_(camera_id) {}

FrameGrabber::~FrameGrabber() {
    Stop();
}

bool FrameGrabber::Start() {
    // Ensure acquisition is running on the remote module.
    if (!cam_.start(camera_id_)) {
        std::cerr << "[FrameGrabber] Failed to start acquisition on camera "
                  << camera_id_ << ".\n";
        // Continue anyway — it may already be running.
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&FrameGrabber::GrabLoop, this);
    std::cout << "[FrameGrabber] Started grab loop for camera " << camera_id_ << ".\n";
    return true;
}

void FrameGrabber::Stop() {
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool FrameGrabber::WaitFirstFrame(int timeout_ms) {
    std::unique_lock<std::mutex> lk(mutex_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [this] { return has_frame_; });
}

bool FrameGrabber::GetLatestFrame(std::vector<uint8_t>& out_pixels,
                                  int& out_width, int& out_height,
                                  int64_t& out_timestamp_ms) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!has_frame_) return false;
    out_pixels       = pixels_;
    out_width        = width_;
    out_height       = height_;
    out_timestamp_ms = timestamp_ms_;
    return true;
}

void FrameGrabber::GrabLoop() {
    int64_t last_ts_us = -1;

    while (running_.load(std::memory_order_acquire)) {
        // Cheap SHM-only timestamp check — zero gRPC, zero pixel copy.
        // Only proceed to grab() when the camera has actually produced a new frame.
        const int64_t shm_ts = cam_.latest_timestamp_us(camera_id_);
        if (shm_ts <= 0 || shm_ts == last_ts_us) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // New frame detected — now pay the cost of the full gRPC + memcpy.
        auto frame = cam_.grab(camera_id_);
        if (!frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        last_ts_us = frame->timestamp_us;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            pixels_       = std::move(frame->pixels);
            width_        = frame->width;
            height_       = frame->height;
            timestamp_ms_ = frame->timestamp_us / 1000;
            has_frame_    = true;
        }
        cv_.notify_all();
    }
}
