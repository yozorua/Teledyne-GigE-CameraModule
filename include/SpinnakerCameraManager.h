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
// FrameSnapshot – heap copy of one image queued for async disk writing
// ─────────────────────────────────────────────────────────────────────────────

struct FrameSnapshot {
    std::vector<uint8_t> data;
    uint32_t             width{};
    uint32_t             height{};
    uint32_t             channels{};
    int32_t              camera_id{-1};
    std::string          save_path;
};

// ─────────────────────────────────────────────────────────────────────────────
// CameraInfo – live state snapshot returned by GetCameraInfo()
// ─────────────────────────────────────────────────────────────────────────────

struct CameraInfo {
    int32_t     camera_id{-1};
    std::string model_name;
    std::string serial;
    std::string ip_address;   // dotted-decimal, empty if not a GigE camera
    int32_t     width{0};
    int32_t     height{0};
    int32_t     offset_x{0};
    int32_t     offset_y{0};
    int32_t     binning_h{1};
    int32_t     binning_v{1};
    float       exposure_us{0.0f};
    float       gain_db{0.0f};
    float       fps{0.0f};
    bool        acquiring{false};
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

    /// Enumerates cameras and calls Init() on each so the NodeMap is accessible
    /// without needing to start acquisition first.
    bool Initialize(const std::string& save_directory = ".");

    void Shutdown();

    // ── Acquisition control ───────────────────────────────────────────────────

    /// @param camera_id  0-based index, or -1 to start all cameras.
    bool StartAcquisition(int32_t camera_id = -1);

    /// @param camera_id  0-based index, or -1 to stop all cameras.
    bool StopAcquisition(int32_t camera_id = -1);

    // ── Parameter control ─────────────────────────────────────────────────────

    /// Sets a GenICam node on the specified camera (or all cameras if -1).
    /// Float-typed nodes receive @p float_value; integer-typed nodes receive
    /// @p int_value.  Returns true if at least one camera accepted the change.
    bool SetParameter(const std::string& param_name,
                      float              float_value,
                      int32_t            int_value,
                      int32_t            camera_id = -1);

    // ── Disk save ─────────────────────────────────────────────────────────────

    /// Flags the next captured frame for disk write (non-blocking).
    void TriggerDiskSave();

    /// Changes the directory where saved frames are written.  Thread-safe.
    void SetSaveDirectory(const std::string& path);

    // ── Status queries ────────────────────────────────────────────────────────

    int32_t GetConnectedCameraCount() const;

    /// Aggregate FPS: sum of per-camera FPS values.
    float GetCurrentFPS() const;

    /// True if at least one camera is actively acquiring.
    bool IsAcquiring() const;

    bool IsCameraAcquiring(int32_t camera_id) const;

    /// Reads live GenICam nodes + cached per-camera FPS into @p info.
    /// Requires the camera to be initialized (i.e. after Initialize() succeeds).
    bool GetCameraInfo(int32_t camera_id, CameraInfo& info);

private:
    // ── Per-camera helpers ────────────────────────────────────────────────────
    bool StartCamera(int32_t camera_id);
    bool StopCamera(int32_t camera_id);

    void ConfigureCamera(Spinnaker::CameraPtr& camera);
    void CameraAcquisitionThread(Spinnaker::CameraPtr camera, int32_t camera_id);
    void DiskSaveLoop();
    void RecordFrameTime(int32_t camera_id);

    SharedMemoryManager&              shm_;
    Spinnaker::SystemPtr              system_;
    Spinnaker::CameraList             cam_list_;
    std::vector<Spinnaker::CameraPtr> cameras_;

    // Per-camera acquisition threads and running flags (indexed by camera_id).
    // Using a fixed array so individual threads can be joined by camera_id.
    std::thread       acq_threads_[MAX_CAMERAS];
    std::atomic<bool> camera_acquiring_[MAX_CAMERAS]{};

    // ── Disk-save queue ───────────────────────────────────────────────────────
    std::string               save_directory_;
    std::mutex                save_dir_mutex_;  // guards save_directory_
    std::queue<FrameSnapshot> save_queue_;
    std::mutex                save_mutex_;      // guards save_queue_
    std::condition_variable   save_cv_;
    std::atomic<bool>         save_thread_running_{false};
    std::thread               save_thread_;
    std::atomic<bool>         pending_save_{false};

    // ── Per-camera FPS tracking ───────────────────────────────────────────────
    static constexpr std::size_t FPS_WINDOW = 30;
    std::atomic<float>  camera_fps_[MAX_CAMERAS]{};
    std::mutex          camera_fps_mutex_[MAX_CAMERAS];
    std::deque<int64_t> camera_frame_times_[MAX_CAMERAS];
};
