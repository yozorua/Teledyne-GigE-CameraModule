#pragma once

#include "gige_camera.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// Polls SHM timestamp for one camera in a background thread and tracks FPS.
// The render thread calls IsNewFrame() then TryPinLatest() for zero-copy access.
class CameraFeed {
public:
    CameraFeed(const std::string& grpc_addr, int32_t camera_id);
    ~CameraFeed();

    CameraFeed(const CameraFeed&)            = delete;
    CameraFeed& operator=(const CameraFeed&) = delete;

    void Start();
    void Stop();

    // Returns true if a frame newer than the last call arrived; clears the flag.
    bool IsNewFrame();

    // Pin the latest SHM slot for zero-copy direct access (no pixel copy).
    // Returns nullopt if RW SHM mapping is unavailable (no admin).
    // Caller MUST call ReleasePin(slot._slot_idx) after uploading.
    std::optional<GigECamera::PinnedSlot> TryPinLatest();
    void ReleasePin(int32_t slot_idx);

    // Fallback: grab into a caller-supplied GigeFrame (reuses pixel buffer).
    // Used when TryPinLatest() returns nullopt.
    bool GrabInto(GigeFrame& out);

    float   GetFps()      const;
    int32_t GetCameraId() const { return camera_id_; }
    bool    UsingDirectGrab()   { return cam_.is_direct_grab_active(); }

    // Blocking gRPC call — call infrequently (not every frame).
    std::optional<GigeCameraInfo> QueryInfo();

private:
    void GrabLoop();

    GigECamera  cam_;
    int32_t     camera_id_;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> frame_new_{false};

    mutable std::mutex  fps_mutex_;
    std::deque<int64_t> frame_times_us_;
    float               fps_{0.f};
    static constexpr int kFpsWindow = 30;
};
