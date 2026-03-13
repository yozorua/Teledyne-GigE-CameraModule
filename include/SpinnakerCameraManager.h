#pragma once

#include "Spinnaker.h"
#include "SharedMemoryManager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// FrameSnapshot – a heap copy of one image for async disk writing
// ─────────────────────────────────────────────────────────────────────────────

struct FrameSnapshot {
    std::vector<uint8_t> data;
    uint32_t             width{};
    uint32_t             height{};
    uint32_t             channels{};
    std::string          save_path;
};

// ─────────────────────────────────────────────────────────────────────────────
// SpinnakerCameraManager
// ─────────────────────────────────────────────────────────────────────────────

class SpinnakerCameraManager {
public:
    explicit SpinnakerCameraManager(SharedMemoryManager& shm);
    ~SpinnakerCameraManager();

    SpinnakerCameraManager(const SpinnakerCameraManager&)            = delete;
    SpinnakerCameraManager& operator=(const SpinnakerCameraManager&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /// Initialises the Spinnaker system and enumerates cameras.
    /// @param save_directory  Directory for disk-save frames (default: ".")
    bool Initialize(const std::string& save_directory = ".");

    void Shutdown();

    // ── Acquisition control ───────────────────────────────────────────────────

    /// Configures all cameras and spawns one acquisition thread per camera.
    bool StartAcquisition();

    /// Signals all acquisition threads to stop and joins them.
    bool StopAcquisition();

    // ── Parameter control ─────────────────────────────────────────────────────

    /// Maps @p param_name to a GenICam node on every connected camera.
    /// Float-typed nodes receive @p float_value; integer-typed nodes receive
    /// @p int_value.  Returns true if at least one camera accepted the change.
    bool SetParameter(const std::string& param_name,
                      float              float_value,
                      int32_t            int_value);

    // ── Disk save ─────────────────────────────────────────────────────────────

    /// Flags the next captured frame to be deep-copied and queued for disk I/O.
    /// Non-blocking: the actual write happens on a dedicated background thread.
    void TriggerDiskSave();

    // ── Status queries ────────────────────────────────────────────────────────

    int32_t GetConnectedCameraCount() const;
    float   GetCurrentFPS()           const;
    bool    IsAcquiring()             const { return acquiring_.load(std::memory_order_acquire); }

private:
    void ConfigureCamera(Spinnaker::CameraPtr& camera);
    void CameraAcquisitionThread(Spinnaker::CameraPtr camera, int32_t camera_id);
    void DiskSaveLoop();
    void RecordFrameTime();

    SharedMemoryManager&              shm_;
    Spinnaker::SystemPtr              system_;
    Spinnaker::CameraList             cam_list_;
    std::vector<Spinnaker::CameraPtr> cameras_;
    std::vector<std::thread>          acq_threads_;

    std::atomic<bool> acquiring_{false};

    // ── Disk-save queue ───────────────────────────────────────────────────────
    std::string                save_directory_;
    std::queue<FrameSnapshot>  save_queue_;
    std::mutex                 save_mutex_;
    std::condition_variable    save_cv_;
    std::atomic<bool>          save_thread_running_{false};
    std::thread                save_thread_;

    /// Set to true by TriggerDiskSave(); cleared by the acquisition thread
    /// after it enqueues one snapshot.
    std::atomic<bool>          pending_save_{false};

    // ── FPS tracking (ring-buffer of frame timestamps) ────────────────────────
    static constexpr std::size_t FPS_WINDOW = 30;
    std::atomic<float>           current_fps_{0.0f};
    std::mutex                   fps_mutex_;
    std::deque<int64_t>          frame_times_us_;   // microseconds since epoch
};
