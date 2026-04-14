#include "SharedMemoryManager.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <new>

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

bool SharedMemoryManager::Initialize(int32_t width, int32_t height, int32_t channels) {
    if (initialized_) return true;

    const std::size_t image_size = static_cast<std::size_t>(width) * height * channels;
    const std::size_t total_size = sizeof(SharedMemoryHeader) + image_size * POOL_SIZE;

    mapping_handle_ = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        static_cast<DWORD>(total_size >> 32),
        static_cast<DWORD>(total_size & 0xFFFFFFFFULL),
        SHM_NAME);

    if (mapping_handle_ == NULL) {
        std::cerr << "[SharedMemory] CreateFileMapping failed: " << GetLastError() << '\n';
        return false;
    }

    mapped_view_ = MapViewOfFile(mapping_handle_, FILE_MAP_ALL_ACCESS, 0, 0, total_size);
    if (!mapped_view_) {
        std::cerr << "[SharedMemory] MapViewOfFile failed: " << GetLastError() << '\n';
        CloseHandle(mapping_handle_);
        mapping_handle_ = NULL;
        return false;
    }

    ZeroMemory(mapped_view_, total_size);

    header_ = new (mapped_view_) SharedMemoryHeader{};
    header_->image_width       = width;
    header_->image_height      = height;
    header_->image_channels    = channels;
    header_->single_image_size = image_size;
    header_->pool_size         = POOL_SIZE;
    header_->latest_buffer_index.store(-1, std::memory_order_relaxed);

    for (int i = 0; i < MAX_CAMERAS; ++i)
        header_->latest_buffer_per_camera[i].store(-1, std::memory_order_relaxed);

    for (int i = 0; i < POOL_SIZE; ++i) {
        header_->reference_counts[i].store(0, std::memory_order_relaxed);
        header_->buffer_camera_id[i] = -1;
        header_->buffer_width[i]     = 0;
        header_->buffer_height[i]    = 0;
    }

    image_data_base_ = reinterpret_cast<uint8_t*>(mapped_view_) + sizeof(SharedMemoryHeader);
    initialized_     = true;

    std::cout << "[SharedMemory] Initialised. "
              << POOL_SIZE << " buffers × " << image_size << " bytes = "
              << total_size << " bytes total.\n";
    return true;
}

void SharedMemoryManager::SetNumCameras(int32_t n) {
    if (header_) header_->num_cameras = n;
}

void SharedMemoryManager::Shutdown() {
    if (mapped_view_) {
        UnmapViewOfFile(mapped_view_);
        mapped_view_     = nullptr;
        header_          = nullptr;
        image_data_base_ = nullptr;
    }
    if (mapping_handle_ != NULL) {
        CloseHandle(mapping_handle_);
        mapping_handle_ = NULL;
    }
    initialized_ = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Producer API
// ─────────────────────────────────────────────────────────────────────────────

int32_t SharedMemoryManager::ClaimFreeBuffer(int32_t camera_id) {
    // Each camera owns an exclusive, non-overlapping slice of the pool.
    // This makes cross-camera buffer recycling structurally impossible:
    // camera 1 can never claim a slot that latest_buffer_per_camera[0] still
    // points to, which eliminates the routing race entirely.
    static_assert(POOL_SIZE % MAX_CAMERAS == 0,
                  "POOL_SIZE must be divisible by MAX_CAMERAS");
    const int32_t slots_per_cam = POOL_SIZE / MAX_CAMERAS;
    const int32_t range_start   = camera_id * slots_per_cam;

    // Round-robin within this camera's range, starting after the slot it last
    // published so we avoid immediately overwriting the most recent frame.
    const int32_t last = header_->latest_buffer_per_camera[camera_id].load(
        std::memory_order_acquire);
    const int32_t offset = (last >= range_start && last < range_start + slots_per_cam)
                         ? (last - range_start + 1) % slots_per_cam
                         : 0;

    for (int32_t i = 0; i < slots_per_cam; ++i) {
        const int32_t candidate = range_start + (offset + i) % slots_per_cam;
        int32_t expected = 0;

        if (header_->reference_counts[candidate].compare_exchange_strong(
                expected, SHM_WRITING,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return candidate;
        }
    }
    return -1;
}

uint8_t* SharedMemoryManager::GetBufferPtr(int32_t index) {
    assert(index >= 0 && index < POOL_SIZE);
    return image_data_base_ + (static_cast<std::size_t>(index) * header_->single_image_size);
}

void SharedMemoryManager::PublishBuffer(int32_t index, int32_t camera_id,
                                        int32_t actual_width, int32_t actual_height) {
    assert(index >= 0 && index < POOL_SIZE);
    assert(camera_id >= 0 && camera_id < MAX_CAMERAS);

    // Tag the buffer with its source camera and actual pixel dimensions.
    // Dimensions may differ from the SHM header's image_width/image_height
    // when the camera ROI was changed after initialisation.
    header_->buffer_camera_id[index] = camera_id;
    header_->buffer_width[index]     = actual_width;
    header_->buffer_height[index]    = actual_height;

    // Release-store: image bytes are visible before refcount or index updates.
    header_->reference_counts[index].store(0, std::memory_order_release);

    // Update per-camera latest.
    header_->latest_buffer_per_camera[camera_id].store(index, std::memory_order_release);

    // Update global latest (most recent from any camera).
    header_->latest_buffer_index.store(index, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// Consumer API
// ─────────────────────────────────────────────────────────────────────────────

/// Internal helper: pin a specific index atomically.
/// Returns the index on success, -1 on failure.
static int32_t PinBuffer(SharedMemoryHeader* hdr, int32_t index,
                         const std::atomic<int32_t>& latest_source) {
    constexpr int MAX_RETRIES = 32;
    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        int32_t current = hdr->reference_counts[index].load(std::memory_order_acquire);
        if (current < 0) continue; // SHM_WRITING — producer is mid-copy

        if (hdr->reference_counts[index].compare_exchange_weak(
                current, current + 1,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {

            // Confirm the index source hasn't changed while we were acquiring.
            if (latest_source.load(std::memory_order_acquire) != index) {
                hdr->reference_counts[index].fetch_sub(1, std::memory_order_release);
                continue; // stale — retry will re-read the caller's source
            }
            return index;
        }
    }
    return -1;
}

int32_t SharedMemoryManager::AcquireLatestFrame(int32_t camera_id) {
    constexpr int MAX_RETRIES = 32;

    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        int32_t index;

        if (camera_id < 0) {
            // Any camera: use global latest.
            index = header_->latest_buffer_index.load(std::memory_order_acquire);
            if (index < 0 || index >= POOL_SIZE) return -1;
            int32_t result = PinBuffer(header_, index, header_->latest_buffer_index);
            if (result >= 0) return result;
        } else {
            // Specific camera.
            if (camera_id >= MAX_CAMERAS) return -1;
            index = header_->latest_buffer_per_camera[camera_id].load(std::memory_order_acquire);
            if (index < 0 || index >= POOL_SIZE) return -1;

            // Structural range check: with per-camera pool partitioning, a
            // valid slot for camera_id must lie in its exclusive range.  A
            // stale pointer left over from before partitioning was enforced
            // would fail here immediately without needing to pin.
            const int32_t slots_per_cam = POOL_SIZE / MAX_CAMERAS;
            if (index < camera_id * slots_per_cam ||
                index >= (camera_id + 1) * slots_per_cam) {
                return -1;
            }

            int32_t result = PinBuffer(header_, index,
                                       header_->latest_buffer_per_camera[camera_id]);
            if (result < 0) continue;

            // Final safety check: buffer_camera_id must match.  With exclusive
            // per-camera slots this should always pass; kept as a hard guard.
            if (header_->buffer_camera_id[index] != camera_id) {
                header_->reference_counts[index].fetch_sub(1, std::memory_order_release);
                continue;
            }
            return result;
        }
        // Falls through only when the any-camera path's PinBuffer failed; retry.
    }
    return -1;
}

void SharedMemoryManager::ReleaseFrame(int32_t index) {
    if (index < 0 || index >= POOL_SIZE) return;
    header_->reference_counts[index].fetch_sub(1, std::memory_order_release);
}
