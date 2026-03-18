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
    int64_t last_ts = -1;

    while (running_.load(std::memory_order_acquire)) {
        auto frame = cam_.grab(camera_id_);
        if (!frame) {
            // No frame available yet — brief wait before retry.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Skip duplicate frames (same timestamp).
        if (frame->timestamp_ms == last_ts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        last_ts = frame->timestamp_ms;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            pixels_       = std::move(frame->pixels);
            width_        = frame->width;
            height_       = frame->height;
            timestamp_ms_ = frame->timestamp_ms;
            has_frame_    = true;
        }
        cv_.notify_all();
    }
}
