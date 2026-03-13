#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int32_t POOL_SIZE   = 20;  // 5 slots × MAX_CAMERAS
static constexpr int32_t MAX_CAMERAS = 4;   // maximum supported cameras
static constexpr LPCSTR  SHM_NAME    = "Global\\CameraImageBufferPool";

/// Sentinel value stored in reference_counts[i] while the producer is writing
/// into buffer i.  Consumers skip buffers with this value.
static constexpr int32_t SHM_WRITING = -1;

// ─────────────────────────────────────────────────────────────────────────────
// Shared Memory Header
// ─────────────────────────────────────────────────────────────────────────────

/// Placed at offset 0 of the shared memory block.
///
/// NOTE: std::atomic<int32_t> is always lock-free on MSVC/x64 and uses
/// LOCK-prefixed CPU instructions, making it safe to share across processes
/// that map the same physical pages.  This is well-defined on Windows/MSVC
/// even though the C++ standard does not guarantee it for all platforms.
struct SharedMemoryHeader {
    /// Global latest: index of the most recently published buffer from ANY camera,
    /// or -1 if no frame has been written yet.
    std::atomic<int32_t> latest_buffer_index{-1};

    /// Per-camera latest: latest_buffer_per_camera[cam_id] holds the index of the
    /// most recently published buffer from camera cam_id, or -1 if none.
    std::atomic<int32_t> latest_buffer_per_camera[MAX_CAMERAS];

    int32_t     image_width{0};
    int32_t     image_height{0};
    int32_t     image_channels{0};
    std::size_t single_image_size{0};   // width * height * channels (bytes)
    int32_t     pool_size{POOL_SIZE};
    int32_t     num_cameras{0};         // set by the producer at initialisation

    /// Which camera produced each buffer (-1 = not yet written).
    int32_t     buffer_camera_id[POOL_SIZE];

    /// Per-buffer state:
    ///   SHM_WRITING (-1) – producer is copying image data into this buffer
    ///   0                – buffer is free (no readers, not being written)
    ///   N > 0            – N consumer processes are currently reading this buffer
    std::atomic<int32_t> reference_counts[POOL_SIZE]{};
};

static_assert(
    std::atomic<int32_t>::is_always_lock_free,
    "std::atomic<int32_t> must be lock-free for correct cross-process use");

// ─────────────────────────────────────────────────────────────────────────────
// SharedMemoryManager
// ─────────────────────────────────────────────────────────────────────────────

class SharedMemoryManager {
public:
    SharedMemoryManager()  = default;
    ~SharedMemoryManager() { Shutdown(); }

    SharedMemoryManager(const SharedMemoryManager&)            = delete;
    SharedMemoryManager& operator=(const SharedMemoryManager&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /// Creates the shared memory block (producer / camera-module side).
    /// Consumer processes open the same name with OpenFileMapping().
    /// Requires SeCreateGlobalPrivilege (run the process as Administrator).
    bool Initialize(int32_t width, int32_t height, int32_t channels);

    /// Called after cameras are enumerated so consumers can read num_cameras.
    void SetNumCameras(int32_t n);

    void Shutdown();

    // ── Producer API ──────────────────────────────────────────────────────────

    /// Scans the pool for a buffer whose reference_count is 0 and atomically
    /// claims it by setting reference_count to SHM_WRITING.
    /// @return Buffer index [0, POOL_SIZE), or -1 if every buffer is busy.
    int32_t ClaimFreeBuffer();

    /// Direct pointer into shared memory for buffer @p index.
    /// Only call this after ClaimFreeBuffer() returns @p index.
    uint8_t* GetBufferPtr(int32_t index);

    /// Releases a previously claimed buffer and makes it visible to consumers.
    /// Updates both the global latest_buffer_index and latest_buffer_per_camera[camera_id].
    void PublishBuffer(int32_t index, int32_t camera_id);

    // ── Consumer API (called from gRPC handlers) ──────────────────────────────

    /// Atomically pins the latest buffer for the requested camera and returns its index.
    /// @param camera_id  0-based camera index, or -1 to get the latest from any camera.
    /// @return Buffer index [0, POOL_SIZE), or -1 if no frame is available.
    int32_t AcquireLatestFrame(int32_t camera_id = -1);

    /// Decrements reference_count[index], allowing the producer to reuse it.
    void ReleaseFrame(int32_t index);

    SharedMemoryHeader* GetHeader() { return header_; }

private:
    HANDLE              mapping_handle_{NULL};
    void*               mapped_view_{nullptr};
    SharedMemoryHeader* header_{nullptr};
    uint8_t*            image_data_base_{nullptr};
    bool                initialized_{false};
};
