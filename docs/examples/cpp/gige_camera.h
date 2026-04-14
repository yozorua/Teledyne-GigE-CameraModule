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
 *       // frame->width, frame->height, frame->camera_id, frame->timestamp_ms
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

static constexpr int32_t POOL_SIZE   = 32;  // 8 slots × MAX_CAMERAS
static constexpr int32_t MAX_CAMERAS = 4;
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

} // namespace gige_detail

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// A single camera frame returned by GigECamera::grab().
struct GigeFrame {
    /// RGB8 pixel data, R-G-B interleaved.
    /// Total bytes = width * height * channels (channels == 3).
    std::vector<uint8_t> pixels;
    int32_t width{0};
    int32_t height{0};
    int32_t channels{3};
    int32_t camera_id{-1};
    int64_t timestamp_ms{0};
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
        out.camera_id   = resp.camera_id();
        out.model_name  = resp.model_name();
        out.serial      = resp.serial();
        out.ip_address  = resp.ip_address();
        out.width       = resp.width();
        out.height      = resp.height();
        out.offset_x    = resp.offset_x();
        out.offset_y    = resp.offset_y();
        out.binning_h   = resp.binning_h();
        out.binning_v   = resp.binning_v();
        out.exposure_us = resp.exposure_us();
        out.gain_db     = resp.gain_db();
        out.gamma       = resp.gamma();
        out.black_level = resp.black_level();
        out.frame_rate    = resp.frame_rate();
        out.fps           = resp.fps();
        out.acquiring     = resp.acquiring();
        out.exposure_auto = resp.exposure_auto();
        out.gain_auto     = resp.gain_auto();
        return out;
    }

    // ── Frame acquisition ─────────────────────────────────────────────────────

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
     *       last_ts = frame->timestamp_ms;
     *       // ... process frame ...
     *   }
     * @endcode
     *
     * @param camera_id   0-based camera index.
     * @param last_ts     Timestamp of the last processed frame (0 = accept any).
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
            if (frame && frame->timestamp_ms != last_ts)
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
            if (frame && frame->timestamp_ms != last_ts)
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

    /// Queue the next frame from camera_id (or any camera) for JPEG disk save.
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
        const int32_t ch  = shm_.header()->image_channels;

        GigeFrame frame;
        frame.width        = w;
        frame.height       = h;
        frame.channels     = ch;
        frame.camera_id    = meta.camera_id();
        frame.timestamp_ms = meta.timestamp();

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
};
