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
// RawFrame – one raw Bayer frame handed from the acquisition thread to the
//            per-camera debayer thread.  A single slot per camera is used
//            (latest-wins): if the debayer thread is still busy when the next
//            frame arrives the new frame simply overwrites the slot.
// ─────────────────────────────────────────────────────────────────────────────

struct RawFrame {
    std::vector<uint8_t>        data;
    int32_t                     width{0};
    int32_t                     height{0};
    Spinnaker::PixelFormatEnums pixel_format{};
    int64_t                     timestamp_us{0};  // system_clock µs at capture time
    bool                        pending{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// FrameSnapshot – heap copy of one debayered RGB image queued for disk writing
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
    float       gamma{1.0f};
    float       black_level{0.0f};
    float       frame_rate{0.0f};   // AcquisitionFrameRate; 0 if node unavailable
    std::string exposure_auto;      // "Off" | "Once" | "Continuous"
    std::string gain_auto;          // "Off" | "Once" | "Continuous"
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
    /// - Enumeration nodes (ExposureAuto, GainAuto, …): pass the entry name in
    ///   @p string_value (e.g. "Continuous", "Once", "Off").
    /// - Float nodes (ExposureTime, Gain, …): pass @p float_value.
    /// - Integer nodes (Width, Height, …): pass @p int_value.
    /// Returns true if at least one camera accepted the change.
    bool SetParameter(const std::string& param_name,
                      float              float_value,
                      int32_t            int_value,
                      int32_t            camera_id    = -1,
                      const std::string& string_value = {});

    // ── Disk save ─────────────────────────────────────────────────────────────

    /// Flags the next captured frame from @p camera_id for disk write
    /// (non-blocking).  Pass -1 to accept a frame from any camera.
    void TriggerDiskSave(int32_t camera_id = -1);

    /// Changes the directory where saved frames are written.  Thread-safe.
    void SetSaveDirectory(const std::string& path);

    // ── Status queries ────────────────────────────────────────────────────────

    int32_t GetConnectedCameraCount() const;

    /// Queries each connected camera for its maximum sensor resolution and
    /// returns the largest width and height seen across all cameras.  Call
    /// this after Initialize() and before SharedMemoryManager::Initialize()
    /// so the SHM buffer slots are large enough for every camera's output.
    /// Falls back to @p fallback_width / @p fallback_height if no cameras
    /// are found or the nodes are unavailable.
    void GetMaxImageDimensions(int32_t& out_width, int32_t& out_height,
                               int32_t  fallback_width  = 1920,
                               int32_t  fallback_height = 1080);

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
    void DebayerThread(int32_t camera_id);
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

    // Per-camera debayer pipeline.
    // The acquisition thread copies raw Bayer bytes into raw_frames_[cam_id]
    // (latest-wins single slot) and signals raw_cv_[cam_id].
    // DebayerThread picks up the raw frame, converts to RGB8, and writes to SHM.
    RawFrame                raw_frames_[MAX_CAMERAS];
    std::mutex              raw_mutex_[MAX_CAMERAS];
    std::condition_variable raw_cv_[MAX_CAMERAS];
    std::thread             debayer_threads_[MAX_CAMERAS];
    std::atomic<bool>       debayer_running_[MAX_CAMERAS]{};

    // ── Disk-save queue ───────────────────────────────────────────────────────
    std::string               save_directory_;
    std::mutex                save_dir_mutex_;  // guards save_directory_
    std::queue<FrameSnapshot> save_queue_;
    std::mutex                save_mutex_;      // guards save_queue_
    std::condition_variable   save_cv_;
    std::atomic<bool>         save_thread_running_{false};
    std::thread               save_thread_;
    // -2 = no pending save; -1 = save from any camera; 0+ = specific camera_id.
    static constexpr int32_t  SAVE_IDLE = -2;
    std::atomic<int32_t>      pending_save_camera_id_{SAVE_IDLE};

    // ── Per-camera channel order ──────────────────────────────────────────────
    // true (default) = swap R↔B after RGB8 debayer → BGR8 in SHM.
    // false          = keep RGB8 as-is.
    // Controlled at runtime via SetParameter("ChannelOrder", …, "BGR"/"RGB").
    std::atomic<bool> swap_rb_[MAX_CAMERAS]{};

    // ── Per-camera debayer mode ───────────────────────────────────────────────
    // false (default) = debayer to RGB8/BGR8 before writing to SHM.
    // true            = write raw Bayer bytes (1 ch) directly to SHM.
    // Controlled at runtime via SetParameter("DebayerMode", …, "On"/"Off").
    std::atomic<bool> skip_debayer_[MAX_CAMERAS]{};

    // ── Per-camera FPS tracking ───────────────────────────────────────────────
    static constexpr std::size_t FPS_WINDOW = 30;
    std::atomic<float>  camera_fps_[MAX_CAMERAS]{};
    std::mutex          camera_fps_mutex_[MAX_CAMERAS];
    std::deque<int64_t> camera_frame_times_[MAX_CAMERAS];
};
