/**
 * gige_camera.h
 * ~~~~~~~~~~~~~
 * Single-header C++17 wrapper for GigECameraModule.
 *
 * Hides gRPC boilerplate and shared-memory layout details.
 * Include this file and link against gRPC + protobuf — that is all.
 *
 * Prerequisites
 * -------------
 *   - gRPC / protobuf via vcpkg (x64-windows)
 *   - Proto stubs generated into a known directory (handled by CMakeLists.txt)
 *   - GigECameraModule.exe running (Administrator)
 *
 * Quick start
 * -----------
 *   #include "gige_camera.h"
 *
 *   GigECamera cam("localhost:50051");
 *   cam.start();
 *
 *   auto frame = cam.grab(0);          // camera 0
 *   if (frame) {
 *       // frame->pixels  R-G-B interleaved, frame->width * frame->height * 3 bytes
 *       // frame->width, frame->height, frame->channels, frame->camera_id
 *       // frame->timestamp_us  — µs since Unix epoch (hardware capture time)
 *   }
 *   cam.stop();
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <grpcpp/grpcpp.h>
#include "camera_service.grpc.pb.h"   // generated — path set by CMakeLists.txt

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Internal: Shared Memory layout
// Mirrors include/SharedMemoryManager.h exactly.
// ─────────────────────────────────────────────────────────────────────────────

namespace gige_detail {

static constexpr int32_t POOL_SIZE   = 40;  // 20 slots × MAX_CAMERAS
static constexpr int32_t MAX_CAMERAS = 2;
static constexpr LPCSTR  SHM_NAME    = "Global\\CameraImageBufferPool";

struct ShmHeader {
    std::atomic<int32_t> latest_buffer_index{-1};
    std::atomic<int32_t> latest_buffer_per_camera[MAX_CAMERAS];
    int32_t     image_width{0};
    int32_t     image_height{0};
    int32_t     image_channels{0};
    std::size_t single_image_size{0};
    int32_t     pool_size{POOL_SIZE};
    int32_t     num_cameras{0};
    int32_t     buffer_camera_id[POOL_SIZE];
    int32_t     buffer_width[POOL_SIZE];
    int32_t     buffer_height[POOL_SIZE];
    int32_t     buffer_channels[POOL_SIZE];      // 1 = Mono/raw Bayer, 3 = BGR/RGB
    int64_t     buffer_timestamp_us[POOL_SIZE];  // µs since Unix epoch (hardware clock)
    std::atomic<int32_t> reference_counts[POOL_SIZE]{};
};

static constexpr std::size_t IMAGE_DATA_OFFSET = sizeof(ShmHeader);

class ShmReader {
public:
    ShmReader() = default;
    ~ShmReader() { close(); }

    ShmReader(const ShmReader&)            = delete;
    ShmReader& operator=(const ShmReader&) = delete;

    bool open() {
        mapping_ = OpenFileMappingA(FILE_MAP_READ, FALSE, SHM_NAME);
        if (!mapping_) return false;
        view_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
        if (!view_) { CloseHandle(mapping_); mapping_ = nullptr; return false; }
        header_    = reinterpret_cast<const ShmHeader*>(view_);
        data_base_ = reinterpret_cast<const uint8_t*>(view_) + IMAGE_DATA_OFFSET;
        return true;
    }

    bool is_open() const { return view_ != nullptr; }

    const ShmHeader* header() const { return header_; }

    /// Returns pointer to the start of buffer slot idx.
    /// Valid only while the gRPC server has the buffer pinned.
    const uint8_t* buffer_ptr(int32_t idx) const {
        return data_base_ + static_cast<std::size_t>(idx) * header_->single_image_size;
    }

    void close() {
        if (view_)    { UnmapViewOfFile(view_);  view_    = nullptr; }
        if (mapping_) { CloseHandle(mapping_);   mapping_ = nullptr; }
        header_    = nullptr;
        data_base_ = nullptr;
    }

private:
    HANDLE           mapping_{nullptr};
    void*            view_{nullptr};
    const ShmHeader* header_{nullptr};
    const uint8_t*   data_base_{nullptr};
};

// Read-write variant: opens SHM with write access so atomic pin/unpin
// (reference_counts CAS) can be performed directly without gRPC round-trips.
class ShmReaderRW {
public:
    ShmReaderRW() = default;
    ~ShmReaderRW() { close(); }

    ShmReaderRW(const ShmReaderRW&)            = delete;
    ShmReaderRW& operator=(const ShmReaderRW&) = delete;

    bool open() {
        mapping_ = OpenFileMappingA(FILE_MAP_WRITE, FALSE, SHM_NAME);
        if (!mapping_) return false;
        view_ = MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, 0);
        if (!view_) { CloseHandle(mapping_); mapping_ = nullptr; return false; }
        header_    = reinterpret_cast<ShmHeader*>(view_);
        data_base_ = reinterpret_cast<uint8_t*>(view_) + IMAGE_DATA_OFFSET;
        return true;
    }

    bool is_open()  const { return view_ != nullptr; }
    ShmHeader*     header()     { return header_; }
    const uint8_t* buffer_ptr(int32_t idx) const {
        return data_base_ + static_cast<std::size_t>(idx) * header_->single_image_size;
    }

    void close() {
        if (view_)    { UnmapViewOfFile(view_);  view_    = nullptr; }
        if (mapping_) { CloseHandle(mapping_);   mapping_ = nullptr; }
        header_    = nullptr;
        data_base_ = nullptr;
    }

private:
    HANDLE      mapping_{nullptr};
    void*       view_{nullptr};
    ShmHeader*  header_{nullptr};
    uint8_t*    data_base_{nullptr};
};

} // namespace gige_detail

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// A single camera frame returned by GigECamera::grab().
struct GigeFrame {
    /// Pixel data.  For debayered frames: BGR interleaved (channels == 3).
    /// For raw Bayer frames (DebayerMode=Off): single channel (channels == 1).
    std::vector<uint8_t> pixels;
    int32_t width{0};
    int32_t height{0};
    int32_t channels{3};
    int32_t camera_id{-1};
    int64_t timestamp_us{0};  ///< µs since Unix epoch (hardware capture time)
};

/// Live state snapshot for one camera.
struct GigeCameraInfo {
    int32_t     camera_id{-1};
    std::string model_name;
    std::string serial;
    std::string ip_address;
    int32_t     width{0};
    int32_t     height{0};
    int32_t     offset_x{0};
    int32_t     offset_y{0};
    int32_t     binning_h{1};
    int32_t     binning_v{1};
    float       exposure_us{0.f};
    float       gain_db{0.f};
    float       gamma{1.f};
    float       black_level{0.f};
    float       frame_rate{0.f};
    float       fps{0.f};
    float       ev_compensation{0.f};  ///< AutoExposureEVCompensation; 0 if unavailable
    int64_t     link_speed_bps{0};    ///< DeviceLinkThroughputLimit in bytes/s; 0 if unavailable
    bool        acquiring{false};
    std::string exposure_auto;  ///< "Off" | "Once" | "Continuous"
    std::string gain_auto;      ///< "Off" | "Once" | "Continuous"
};

/// Module-level health snapshot.
struct GigeSystemState {
    std::string status;            ///< "IDLE" | "ACQUIRING" | "PARTIAL" | "ERROR"
    int32_t     connected_cameras{0};
    float       current_fps{0.f};
};


/**
 * GigECamera — simple interface to GigECameraModule.
 *
 * Combines gRPC control + shared-memory frame access into one object.
 * Shared memory is opened lazily on the first grab() call.
 */
class GigECamera {
public:
    /**
     * @param address  gRPC server address, e.g. "localhost:50051".
     */
    explicit GigECamera(const std::string& address = "localhost:50051")
        : channel_(grpc::CreateChannel(address, grpc::InsecureChannelCredentials()))
        , stub_(camaramodule::CameraControl::NewStub(channel_))
    {}

    ~GigECamera() = default;

    GigECamera(const GigECamera&)            = delete;
    GigECamera& operator=(const GigECamera&) = delete;

    // ── System-level control ──────────────────────────────────────────────────

    /// Return current module health.
    GigeSystemState state() {
        grpc::ClientContext ctx;
        camaramodule::Empty    req;
        camaramodule::SystemState resp;
        stub_->GetSystemState(&ctx, req, &resp);
        return { resp.status(), resp.connected_cameras(), resp.current_fps() };
    }

    /**
     * Start acquisition.
     * @param camera_id  0-based index.  -1 (default) starts all cameras.
     */
    bool start(int32_t camera_id = -1) {
        grpc::ClientContext ctx;
        camaramodule::CameraRequest  req;
        camaramodule::CommandStatus  resp;
        req.set_camera_id(camera_id);
        stub_->StartAcquisition(&ctx, req, &resp);
        return resp.success();
    }

    /**
     * Stop acquisition.
     * @param camera_id  0-based index.  -1 (default) stops all cameras.
     */
    bool stop(int32_t camera_id = -1) {
        grpc::ClientContext ctx;
        camaramodule::CameraRequest  req;
        camaramodule::CommandStatus  resp;
        req.set_camera_id(camera_id);
        stub_->StopAcquisition(&ctx, req, &resp);
        return resp.success();
    }

    // ── Camera info ───────────────────────────────────────────────────────────

    /// Return full state snapshot for a single camera.
    std::optional<GigeCameraInfo> info(int32_t camera_id = 0) {
        grpc::ClientContext ctx;
        camaramodule::CameraRequest req;
        camaramodule::CameraState   resp;
        req.set_camera_id(camera_id);
        auto s = stub_->GetCameraInfo(&ctx, req, &resp);
        if (!s.ok()) return std::nullopt;
        GigeCameraInfo out;
        out.camera_id      = resp.camera_id();
        out.model_name     = resp.model_name();
        out.serial         = resp.serial();
        out.ip_address     = resp.ip_address();
        out.width          = resp.width();
        out.height         = resp.height();
        out.offset_x       = resp.offset_x();
        out.offset_y       = resp.offset_y();
        out.binning_h      = resp.binning_h();
        out.binning_v      = resp.binning_v();
        out.exposure_us    = resp.exposure_us();
        out.gain_db        = resp.gain_db();
        out.gamma          = resp.gamma();
        out.black_level    = resp.black_level();
        out.frame_rate     = resp.frame_rate();
        out.fps            = resp.fps();
        out.ev_compensation = resp.ev_compensation();
        out.link_speed_bps = resp.link_speed_bps();
        out.acquiring      = resp.acquiring();
        out.exposure_auto  = resp.exposure_auto();
        out.gain_auto      = resp.gain_auto();
        return out;
    }

    // ── Frame acquisition ─────────────────────────────────────────────────────

    /**
     * Read the hardware timestamp of the latest available frame directly from
     * shared memory — no gRPC round-trip, no pixel copy.
     *
     * Use this in polling loops to detect new frames cheaply before committing
     * to a full grab() call.
     *
     * @param camera_id  0-based camera index; -1 for any camera.
     * @return Timestamp in µs since Unix epoch, or -1 if SHM is not open or
     *         no frame has been published yet.
     */
    int64_t latest_timestamp_us(int32_t camera_id = 0) {
        ensure_shm();
        const auto* hdr = shm_.header();
        const int32_t idx = (camera_id < 0)
            ? hdr->latest_buffer_index.load(std::memory_order_acquire)
            : hdr->latest_buffer_per_camera[camera_id].load(std::memory_order_acquire);
        if (idx < 0 || idx >= hdr->pool_size) return -1;
        return hdr->buffer_timestamp_us[idx];
    }

    /**
     * Grab the latest frame from a specific camera.
     *
     * Pixel data is copied out of shared memory before returning — no explicit
     * release is required.
     *
     * @param camera_id  0-based camera index.
     * @return Frame on success, std::nullopt if no frame is available.
     */
    std::optional<GigeFrame> grab(int32_t camera_id = 0) {
        return grab_impl(camera_id);
    }

    /**
     * High-performance variant of grab(): pins and unpins the SHM buffer
     * directly via atomic CAS, bypassing the two gRPC round-trips that
     * grab() uses.  Reduces per-frame overhead from ~60 ms to ~2 ms for
     * large sensors.
     *
     * Requires FILE_MAP_WRITE access to the SHM (same user, or elevated
     * process).  Falls back to grab() automatically if the RW mapping fails
     * (e.g. insufficient permissions).
     *
     * @param camera_id  0-based camera index; -1 for any camera.
     * @return Frame on success, std::nullopt if no frame is available.
     */
    std::optional<GigeFrame> grab_direct(int32_t camera_id = 0) {
        ensure_shm_rw();
        if (!shm_rw_.is_open())
            return grab_impl(camera_id);   // graceful fallback

        using namespace gige_detail;
        ShmHeader*       hdr           = shm_rw_.header();
        constexpr int    MAX_RETRIES   = 32;
        const int32_t    pool_sz       = hdr->pool_size;
        const int32_t    slots_per_cam = pool_sz / MAX_CAMERAS;

        for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
            int32_t index;
            if (camera_id < 0) {
                index = hdr->latest_buffer_index.load(std::memory_order_acquire);
                if (index < 0 || index >= pool_sz) return std::nullopt;
            } else {
                if (camera_id >= MAX_CAMERAS) return std::nullopt;
                index = hdr->latest_buffer_per_camera[camera_id].load(
                    std::memory_order_acquire);
                if (index < 0 || index >= pool_sz) return std::nullopt;
                // Structural range check (per-camera pool partitioning).
                if (index < camera_id * slots_per_cam ||
                    index >= (camera_id + 1) * slots_per_cam)
                    return std::nullopt;
            }

            // CAS pin: reference_counts[index] : N → N+1 (skip if SHM_WRITING = -1).
            int32_t cur = hdr->reference_counts[index].load(std::memory_order_acquire);
            if (cur < 0) continue;
            if (!hdr->reference_counts[index].compare_exchange_weak(
                    cur, cur + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                continue;

            // Verify index didn't change between our read and the CAS.
            const int32_t verify = (camera_id < 0)
                ? hdr->latest_buffer_index.load(std::memory_order_acquire)
                : hdr->latest_buffer_per_camera[camera_id].load(std::memory_order_acquire);
            if (verify != index) {
                hdr->reference_counts[index].fetch_sub(1, std::memory_order_release);
                continue;
            }

            GigeFrame frame;
            frame.width        = hdr->buffer_width[index];
            frame.height       = hdr->buffer_height[index];
            frame.channels     = hdr->buffer_channels[index];
            frame.camera_id    = (camera_id >= 0) ? camera_id : hdr->buffer_camera_id[index];
            frame.timestamp_us = hdr->buffer_timestamp_us[index];

            const std::size_t n =
                static_cast<std::size_t>(frame.width) * frame.height * frame.channels;
            frame.pixels.resize(n);
            std::memcpy(frame.pixels.data(), shm_rw_.buffer_ptr(index), n);

            // Unpin: decrement refcount so the producer can reclaim this slot.
            hdr->reference_counts[index].fetch_sub(1, std::memory_order_release);
            return frame;
        }
        return std::nullopt;
    }

    /**
     * In-place variant of grab_direct() that reuses the pixel buffer in 'out'
     * across repeated calls, eliminating the per-frame VirtualAlloc + Windows
     * zero-page-fault overhead (~40 ms at 5320×4600).
     *
     * After the first call the output buffer's capacity equals the frame size;
     * subsequent resize() calls are no-ops and memcpy writes to already-committed
     * pages — grab time drops from ~50 ms to ~2 ms.
     *
     * Usage in a polling loop:
     * @code
     *   GigeFrame frame;   // persistent across iterations
     *   while (running) {
     *       if (!cam.grab_direct_into(camera_id, frame)) continue;
     *       frame.pixels.swap(display_buffer);   // O(1), keeps capacity warm
     *   }
     * @endcode
     *
     * @param camera_id  0-based camera index; -1 for any camera.
     * @param out        Frame to fill in-place.  Pixel buffer is reused.
     * @return true on success, false if no frame is available.
     */
    bool grab_direct_into(int32_t camera_id, GigeFrame& out) {
        ensure_shm_rw();
        if (!shm_rw_.is_open()) {
            auto f = grab_impl(camera_id);
            if (!f) return false;
            out = std::move(*f);
            return true;
        }

        using namespace gige_detail;
        ShmHeader*    hdr          = shm_rw_.header();
        constexpr int MAX_RETRIES  = 32;
        const int32_t pool_sz      = hdr->pool_size;
        const int32_t slots_per_cam = pool_sz / MAX_CAMERAS;

        for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
            int32_t index;
            if (camera_id < 0) {
                index = hdr->latest_buffer_index.load(std::memory_order_acquire);
                if (index < 0 || index >= pool_sz) return false;
            } else {
                if (camera_id >= MAX_CAMERAS) return false;
                index = hdr->latest_buffer_per_camera[camera_id].load(
                    std::memory_order_acquire);
                if (index < 0 || index >= pool_sz) return false;
                if (index < camera_id * slots_per_cam ||
                    index >= (camera_id + 1) * slots_per_cam)
                    return false;
            }

            int32_t cur = hdr->reference_counts[index].load(std::memory_order_acquire);
            if (cur < 0) continue;
            if (!hdr->reference_counts[index].compare_exchange_weak(
                    cur, cur + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                continue;

            const int32_t verify = (camera_id < 0)
                ? hdr->latest_buffer_index.load(std::memory_order_acquire)
                : hdr->latest_buffer_per_camera[camera_id].load(
                    std::memory_order_acquire);
            if (verify != index) {
                hdr->reference_counts[index].fetch_sub(1, std::memory_order_release);
                continue;
            }

            out.width        = hdr->buffer_width[index];
            out.height       = hdr->buffer_height[index];
            out.channels     = hdr->buffer_channels[index];
            out.camera_id    = (camera_id >= 0) ? camera_id : hdr->buffer_camera_id[index];
            out.timestamp_us = hdr->buffer_timestamp_us[index];

            const std::size_t n =
                static_cast<std::size_t>(out.width) * out.height * out.channels;
            // resize() is a no-op once the buffer has reached capacity n —
            // no VirtualAlloc, no zero-page faults after the first 2 frames.
            out.pixels.resize(n);
            std::memcpy(out.pixels.data(), shm_rw_.buffer_ptr(index), n);

            hdr->reference_counts[index].fetch_sub(1, std::memory_order_release);
            return true;
        }
        return false;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Zero-copy SHM pin API
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Handle to a pinned SHM slot.
     *
     * `data` points directly into shared memory — no pixel copy.
     * The slot stays pinned (producer cannot reclaim it) until release_pin().
     */
    struct PinnedSlot {
        const uint8_t* data{nullptr};
        int32_t width{0}, height{0}, channels{0};
        int32_t camera_id{-1};
        int64_t timestamp_us{0};
        int32_t _slot_idx{-1};   ///< internal — pass to release_pin()
    };

    /**
     * Pin the latest SHM slot for camera_id and return a raw pointer into SHM.
     *
     * Zero pixel copy: the caller reads or uploads directly from the returned
     * data pointer.  release_pin() MUST be called when done.
     *
     * @return Pinned slot, or std::nullopt if RW SHM mapping is unavailable.
     */
    std::optional<PinnedSlot> pin_latest(int32_t camera_id = 0) {
        ensure_shm_rw();
        if (!shm_rw_.is_open()) return std::nullopt;

        using namespace gige_detail;
        ShmHeader*    hdr          = shm_rw_.header();
        constexpr int MAX_RETRIES  = 32;
        const int32_t pool_sz      = hdr->pool_size;
        const int32_t slots_per_cam = pool_sz / MAX_CAMERAS;

        for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
            int32_t index;
            if (camera_id < 0) {
                index = hdr->latest_buffer_index.load(std::memory_order_acquire);
                if (index < 0 || index >= pool_sz) return std::nullopt;
            } else {
                if (camera_id >= MAX_CAMERAS) return std::nullopt;
                index = hdr->latest_buffer_per_camera[camera_id].load(
                    std::memory_order_acquire);
                if (index < 0 || index >= pool_sz) return std::nullopt;
                if (index < camera_id * slots_per_cam ||
                    index >= (camera_id + 1) * slots_per_cam)
                    return std::nullopt;
            }

            int32_t cur = hdr->reference_counts[index].load(std::memory_order_acquire);
            if (cur < 0) continue;
            if (!hdr->reference_counts[index].compare_exchange_weak(
                    cur, cur + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                continue;

            const int32_t verify = (camera_id < 0)
                ? hdr->latest_buffer_index.load(std::memory_order_acquire)
                : hdr->latest_buffer_per_camera[camera_id].load(
                    std::memory_order_acquire);
            if (verify != index) {
                hdr->reference_counts[index].fetch_sub(1, std::memory_order_release);
                continue;
            }

            PinnedSlot ps;
            ps.data        = shm_rw_.buffer_ptr(index);
            ps.width       = hdr->buffer_width[index];
            ps.height      = hdr->buffer_height[index];
            ps.channels    = hdr->buffer_channels[index];
            ps.camera_id   = (camera_id >= 0) ? camera_id : hdr->buffer_camera_id[index];
            ps.timestamp_us = hdr->buffer_timestamp_us[index];
            ps._slot_idx   = index;
            return ps;   // slot stays pinned; caller must call release_pin()
        }
        return std::nullopt;
    }

    /** Unpin a slot acquired with pin_latest(). */
    void release_pin(int32_t slot_idx) {
        if (slot_idx < 0) return;
        ensure_shm_rw();
        if (!shm_rw_.is_open()) return;
        shm_rw_.header()->reference_counts[slot_idx].fetch_sub(
            1, std::memory_order_release);
    }

    /**
     * Grab the latest frame from whichever camera produced it most recently.
     *
     * @return Frame on success, std::nullopt if no frame is available.
     */
    std::optional<GigeFrame> grab_any() {
        return grab_impl(-1);
    }

    /**
     * Block until a frame newer than last_ts arrives, or timeout expires.
     *
     * Use this in processing loops so you never process the same frame twice:
     * @code
     *   int64_t last_ts = 0;
     *   while (running) {
     *       auto frame = cam.grab_wait(0, last_ts, 500);
     *       if (!frame) continue;       // timeout — try again
     *       last_ts = frame->timestamp_us;
     *       // ... process frame ...
     *   }
     * @endcode
     *
     * @param camera_id   0-based camera index.
     * @param last_ts     Timestamp (µs) of the last processed frame (0 = accept any).
     * @param timeout_ms  Maximum wait in milliseconds.
     * @return New frame, or std::nullopt on timeout.
     */
    std::optional<GigeFrame> grab_wait(int32_t camera_id = 0,
                                       int64_t last_ts   = 0,
                                       int     timeout_ms = 1000)
    {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
        while (clock::now() < deadline) {
            auto frame = grab_impl(camera_id);
            if (frame && frame->timestamp_us != last_ts)
                return frame;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::nullopt;
    }

    /**
     * Blocking any-camera variant of grab_wait().
     *
     * Returns the next new frame from whichever camera produced it most recently.
     */
    std::optional<GigeFrame> grab_any_wait(int64_t last_ts   = 0,
                                           int     timeout_ms = 1000)
    {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
        while (clock::now() < deadline) {
            auto frame = grab_impl(-1);
            if (frame && frame->timestamp_us != last_ts)
                return frame;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::nullopt;
    }

    // ── Parameter control ─────────────────────────────────────────────────────

    /// Set exposure time in microseconds.
    bool set_exposure(float microseconds, int32_t camera_id = -1) {
        return set_float("ExposureTime", microseconds, camera_id);
    }

    /// Set gain in dB.
    bool set_gain(float db, int32_t camera_id = -1) {
        return set_float("Gain", db, camera_id);
    }

    /// Set gamma correction value.
    bool set_gamma(float gamma, int32_t camera_id = -1) {
        return set_float("Gamma", gamma, camera_id);
    }

    /// Set acquisition frame rate.
    bool set_frame_rate(float fps, int32_t camera_id = -1) {
        return set_float("AcquisitionFrameRate", fps, camera_id);
    }

    /**
     * Set auto-exposure EV compensation.
     *
     * Only available when ExposureAuto is "Continuous" or "Once".
     * Typical range: -3.0 to +3.0 EV.
     */
    bool set_ev_compensation(float ev, int32_t camera_id = -1) {
        return set_float("AutoExposureEVCompensation", ev, camera_id);
    }

    /**
     * Set auto-exposure mode.
     *
     * @param mode       "Off"        — fixed exposure (use set_exposure() to set value)
     *                   "Once"       — adjust once, then revert to Off
     *                   "Continuous" — continuously adjusts exposure
     * @param camera_id  0-based index, or -1 for all cameras.
     */
    bool set_exposure_auto(const std::string& mode, int32_t camera_id = -1) {
        return set_enum("ExposureAuto", mode, camera_id);
    }

    /**
     * Set auto-gain mode.
     *
     * @param mode       "Off" | "Once" | "Continuous"
     * @param camera_id  0-based index, or -1 for all cameras.
     */
    bool set_gain_auto(const std::string& mode, int32_t camera_id = -1) {
        return set_enum("GainAuto", mode, camera_id);
    }

    /**
     * Set region of interest.
     *
     * Resets offsets to zero first, then applies width/height, then offsets,
     * to keep all values within sensor bounds throughout.
     */
    bool set_roi(int32_t width, int32_t height,
                 int32_t offset_x = 0, int32_t offset_y = 0,
                 int32_t camera_id = -1)
    {
        bool ok = true;
        ok &= set_int("OffsetX", 0,        camera_id);
        ok &= set_int("OffsetY", 0,        camera_id);
        ok &= set_int("Width",   width,    camera_id);
        ok &= set_int("Height",  height,   camera_id);
        ok &= set_int("OffsetX", offset_x, camera_id);
        ok &= set_int("OffsetY", offset_y, camera_id);
        return ok;
    }

    /**
     * Control raw Bayer passthrough.
     *
     * "Off"  — skip Spinnaker debayering; SHM contains 1-ch raw Bayer bytes.
     *          Disk saves are written as binary .raw files.
     * "On"   — normal debayering (default); SHM contains 3-ch BGR.
     */
    bool set_debayer_mode(const std::string& mode, int32_t camera_id = -1) {
        return set_enum("DebayerMode", mode, camera_id);
    }

    /**
     * Set any GenICam node by name.
     *
     * - Enumeration nodes (ExposureAuto, GainAuto, …): pass the entry name
     *   in string_value (e.g. "Continuous", "Once", "Off").
     * - Float nodes (ExposureTime, Gain, Gamma, …): pass float_value.
     * - Integer nodes (Width, Height, OffsetX, …): pass int_value.
     */
    bool set_param(const std::string& name,
                   float              float_value  = 0.f,
                   int32_t            int_value    = 0,
                   int32_t            camera_id    = -1,
                   const std::string& string_value = {})
    {
        grpc::ClientContext ctx;
        camaramodule::ParameterRequest req;
        camaramodule::CommandStatus    resp;
        req.set_camera_id(camera_id);
        req.set_param_name(name);
        req.set_float_value(float_value);
        req.set_int_value(int_value);
        req.set_string_value(string_value);
        stub_->SetParameter(&ctx, req, &resp);
        return resp.success();
    }

    // ── Disk save ─────────────────────────────────────────────────────────────

    /// Queue the next frame from camera_id (or any camera) for disk save.
    /// Debayered frames are saved as JPEG; raw Bayer frames as binary .raw.
    void save_next(int32_t camera_id = -1) {
        grpc::ClientContext ctx;
        camaramodule::CameraRequest req;
        camaramodule::CommandStatus resp;
        req.set_camera_id(camera_id);
        stub_->TriggerDiskSave(&ctx, req, &resp);
    }

    /// Change the directory where saved frames are written.
    bool set_save_dir(const std::string& path) {
        grpc::ClientContext ctx;
        camaramodule::SaveDirectoryRequest req;
        camaramodule::CommandStatus        resp;
        req.set_path(path);
        stub_->SetSaveDirectory(&ctx, req, &resp);
        return resp.success();
    }

    // ── Timestamp calibration ─────────────────────────────────────────────────

    /**
     * Re-calibrate the camera hardware clock → wall-clock offset.
     *
     * Call this if you notice timestamp drift, or after a long idle period.
     * The server brackets a TimestampLatch command with two system_clock samples
     * and stores the midpoint as the new offset.
     *
     * @param camera_id  0-based index, or -1 (default) for all cameras.
     * @return true if the latch node was available; false means the server
     *         fell back to system_clock timestamps for that camera.
     */
    bool resync_timestamp(int32_t camera_id = -1) {
        grpc::ClientContext ctx;
        camaramodule::CameraRequest req;
        camaramodule::CommandStatus resp;
        req.set_camera_id(camera_id);
        auto s = stub_->ResyncTimestamp(&ctx, req, &resp);
        return s.ok() && resp.success();
    }

private:
    // ── Internal helpers ──────────────────────────────────────────────────────

    void ensure_shm() {
        if (!shm_.is_open()) {
            if (!shm_.open())
                throw std::runtime_error(
                    "Cannot open shared memory 'Global\\CameraImageBufferPool'. "
                    "Is GigECameraModule.exe running?");
        }
    }

    // Best-effort: no exception if write access is denied; grab_direct() falls
    // back to gRPC in that case.
    void ensure_shm_rw() {
        if (!shm_rw_.is_open()) shm_rw_.open();
    }

public:
    // Returns true once grab_direct() has successfully opened the RW SHM mapping.
    // False means it is falling back to gRPC (run both processes as Administrator).
    bool is_direct_grab_active() {
        ensure_shm_rw();
        return shm_rw_.is_open();
    }

private:

    std::optional<GigeFrame> grab_impl(int32_t camera_id) {
        ensure_shm();

        grpc::ClientContext ctx;
        camaramodule::FrameRequest req;
        camaramodule::FrameInfo    meta;
        req.set_camera_id(camera_id);
        auto s = stub_->GetLatestImageFrame(&ctx, req, &meta);
        if (!s.ok()) return std::nullopt;

        const int32_t idx = meta.shared_memory_index();
        const int32_t w   = meta.width();
        const int32_t h   = meta.height();
        // Use per-buffer channel count (may be 1 for raw Bayer, 3 for BGR).
        const int32_t ch  = shm_.header()->buffer_channels[idx];

        GigeFrame frame;
        frame.width        = w;
        frame.height       = h;
        frame.channels     = ch;
        frame.camera_id    = meta.camera_id();
        frame.timestamp_us = meta.timestamp();

        // Copy pixels out of SHM before releasing the pin.
        const std::size_t n = static_cast<std::size_t>(w) * h * ch;
        frame.pixels.resize(n);
        std::memcpy(frame.pixels.data(), shm_.buffer_ptr(idx), n);

        // Release the SHM buffer pin.
        grpc::ClientContext rel_ctx;
        camaramodule::ReleaseRequest rel_req;
        camaramodule::CommandStatus  rel_resp;
        rel_req.set_shared_memory_index(idx);
        stub_->ReleaseImageFrame(&rel_ctx, rel_req, &rel_resp);

        return frame;
    }

    bool set_float(const std::string& name, float value, int32_t camera_id) {
        return set_param(name, value, 0, camera_id);
    }

    bool set_int(const std::string& name, int32_t value, int32_t camera_id) {
        return set_param(name, 0.f, value, camera_id);
    }

    bool set_enum(const std::string& name, const std::string& entry, int32_t camera_id) {
        return set_param(name, 0.f, 0, camera_id, entry);
    }

    std::shared_ptr<grpc::Channel>                     channel_;
    std::unique_ptr<camaramodule::CameraControl::Stub> stub_;
    gige_detail::ShmReader                             shm_;
    gige_detail::ShmReaderRW                           shm_rw_;
};
