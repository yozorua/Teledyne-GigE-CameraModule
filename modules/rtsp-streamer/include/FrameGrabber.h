#pragma once

#include "gige_camera.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// FrameGrabber
//
// Continuously grabs RGB8 frames from GigECameraModule via the gige_camera
// wrapper.  The latest frame is stored; StreamLoop reads it on each tick.
// ─────────────────────────────────────────────────────────────────────────────

class FrameGrabber {
public:
    /// @param grpc_addr   Address of the running GigECameraModule gRPC server.
    /// @param camera_id   0-based camera index to grab from.
    FrameGrabber(const std::string& grpc_addr, int32_t camera_id);
    ~FrameGrabber();

    FrameGrabber(const FrameGrabber&)            = delete;
    FrameGrabber& operator=(const FrameGrabber&) = delete;

    /// Start acquisition on the remote camera and begin the grab loop.
    bool Start();
    void Stop();

    /// Block until the first frame has arrived (or timeout expires).
    /// @return true if a frame is ready.
    bool WaitFirstFrame(int timeout_ms = 5000);

    /// Copy the latest frame into @p out_pixels (RGB8, width*height*3).
    /// @return false if no frame has been received yet.
    bool GetLatestFrame(std::vector<uint8_t>& out_pixels,
                        int& out_width, int& out_height,
                        int64_t& out_timestamp_ms);

    int Width()  const { return width_;  }
    int Height() const { return height_; }

private:
    void GrabLoop();

    GigECamera    cam_;
    int32_t       camera_id_;

    std::thread       thread_;
    std::atomic<bool> running_{false};

    std::mutex              mutex_;
    std::condition_variable cv_;
    std::vector<uint8_t>    pixels_;
    int                     width_{0};
    int                     height_{0};
    int64_t                 timestamp_ms_{0};
    bool                    has_frame_{false};
};
