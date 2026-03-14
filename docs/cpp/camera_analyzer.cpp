/**
 * camera_analyzer.cpp
 *
 * Example consumer for GigECameraModule.
 *
 * Demonstrates:
 *   - Connecting to the gRPC server
 *   - Querying system and camera state
 *   - Grabbing frames via GetLatestImageFrame / ReleaseImageFrame
 *   - Reading raw pixel data directly from Windows Shared Memory
 *   - Computing per-frame statistics (min / max / mean)
 *
 * Build:
 *   cmake -G "Ninja"
 *         -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
 *         -DVCPKG_TARGET_TRIPLET=x64-windows
 *         -DCMAKE_BUILD_TYPE=Release
 *         -B build .
 *   cmake --build build --parallel
 *
 * Run (no Administrator required — SHM is opened read-only):
 *   build\camera_analyzer.exe [grpc_address] [camera_id] [frame_count]
 *   build\camera_analyzer.exe localhost:50051 0 100
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <grpcpp/grpcpp.h>
#include "camera_service.grpc.pb.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Shared Memory layout (mirrors include/SharedMemoryManager.h exactly)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int32_t POOL_SIZE   = 20;
static constexpr int32_t MAX_CAMERAS = 4;
static constexpr LPCSTR  SHM_NAME    = "Global\\CameraImageBufferPool";
static constexpr int32_t SHM_WRITING = -1;

struct SharedMemoryHeader {
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

// ─────────────────────────────────────────────────────────────────────────────
// ShmReader — opens the shared memory block read-only (no admin required)
// ─────────────────────────────────────────────────────────────────────────────

class ShmReader {
public:
    ShmReader() = default;
    ~ShmReader() { close(); }

    ShmReader(const ShmReader&)            = delete;
    ShmReader& operator=(const ShmReader&) = delete;

    bool open() {
        mapping_ = OpenFileMappingA(FILE_MAP_READ, FALSE, SHM_NAME);
        if (!mapping_) {
            std::cerr << "[ShmReader] OpenFileMapping failed: " << GetLastError() << "\n";
            return false;
        }

        view_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
        if (!view_) {
            std::cerr << "[ShmReader] MapViewOfFile failed: " << GetLastError() << "\n";
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }

        header_    = reinterpret_cast<const SharedMemoryHeader*>(view_);
        data_base_ = reinterpret_cast<const uint8_t*>(view_) + sizeof(SharedMemoryHeader);

        std::cout << "[ShmReader] Opened SHM: "
                  << header_->image_width << "x" << header_->image_height
                  << "x" << header_->image_channels
                  << "  pool=" << header_->pool_size
                  << "  cameras=" << header_->num_cameras << "\n";
        return true;
    }

    bool is_open() const { return view_ != nullptr; }

    const SharedMemoryHeader* header() const { return header_; }

    /// Returns a read-only pointer to the pixel data for buffer slot @p idx.
    /// The caller must have already pinned the buffer via gRPC GetLatestImageFrame.
    const uint8_t* buffer_ptr(int32_t idx) const {
        assert(idx >= 0 && idx < POOL_SIZE);
        return data_base_ + static_cast<std::size_t>(idx) * header_->single_image_size;
    }

    void close() {
        if (view_)    { UnmapViewOfFile(view_);    view_    = nullptr; }
        if (mapping_) { CloseHandle(mapping_);     mapping_ = nullptr; }
        header_    = nullptr;
        data_base_ = nullptr;
    }

private:
    HANDLE                    mapping_{nullptr};
    void*                     view_{nullptr};
    const SharedMemoryHeader* header_{nullptr};
    const uint8_t*            data_base_{nullptr};
};

// ─────────────────────────────────────────────────────────────────────────────
// GrpcClient — thin wrapper around the generated gRPC stub
// ─────────────────────────────────────────────────────────────────────────────

class GrpcClient {
public:
    explicit GrpcClient(const std::string& address)
        : channel_(grpc::CreateChannel(address, grpc::InsecureChannelCredentials()))
        , stub_(camaramodule::CameraControl::NewStub(channel_))
    {}

    // -- System state ----------------------------------------------------------

    bool GetSystemState(camaramodule::SystemState& out) {
        grpc::ClientContext ctx;
        camaramodule::Empty req;
        grpc::Status s = stub_->GetSystemState(&ctx, req, &out);
        return check(s, "GetSystemState");
    }

    // -- Camera info -----------------------------------------------------------

    bool GetCameraInfo(int32_t camera_id, camaramodule::CameraState& out) {
        grpc::ClientContext ctx;
        camaramodule::CameraRequest req;
        req.set_camera_id(camera_id);
        grpc::Status s = stub_->GetCameraInfo(&ctx, req, &out);
        return check(s, "GetCameraInfo");
    }

    // -- Acquisition control ---------------------------------------------------

    bool StartAcquisition(int32_t camera_id = -1) {
        grpc::ClientContext ctx;
        camaramodule::CameraRequest req;
        camaramodule::CommandStatus resp;
        req.set_camera_id(camera_id);
        grpc::Status s = stub_->StartAcquisition(&ctx, req, &resp);
        return check(s, "StartAcquisition") && resp.success();
    }

    bool StopAcquisition(int32_t camera_id = -1) {
        grpc::ClientContext ctx;
        camaramodule::CameraRequest req;
        camaramodule::CommandStatus resp;
        req.set_camera_id(camera_id);
        grpc::Status s = stub_->StopAcquisition(&ctx, req, &resp);
        return check(s, "StopAcquisition") && resp.success();
    }

    // -- Parameter control -----------------------------------------------------

    /// Set a float parameter (e.g. ExposureTime, Gain, Gamma, AcquisitionFrameRate)
    bool SetFloatParameter(const std::string& name, float value, int32_t camera_id = -1) {
        grpc::ClientContext ctx;
        camaramodule::ParameterRequest req;
        camaramodule::CommandStatus    resp;
        req.set_camera_id(camera_id);
        req.set_param_name(name);
        req.set_float_value(value);
        grpc::Status s = stub_->SetParameter(&ctx, req, &resp);
        if (!check(s, "SetParameter")) return false;
        if (!resp.success())
            std::cerr << "[GrpcClient] SetParameter failed: " << resp.message() << "\n";
        return resp.success();
    }

    /// Set an integer parameter (e.g. Width, Height, OffsetX, OffsetY)
    bool SetIntParameter(const std::string& name, int32_t value, int32_t camera_id = -1) {
        grpc::ClientContext ctx;
        camaramodule::ParameterRequest req;
        camaramodule::CommandStatus    resp;
        req.set_camera_id(camera_id);
        req.set_param_name(name);
        req.set_int_value(value);
        grpc::Status s = stub_->SetParameter(&ctx, req, &resp);
        if (!check(s, "SetParameter")) return false;
        if (!resp.success())
            std::cerr << "[GrpcClient] SetParameter failed: " << resp.message() << "\n";
        return resp.success();
    }

    // -- Frame acquisition -----------------------------------------------------

    /// Pins the latest buffer for the specified camera (or -1 for any camera).
    /// On success, @p out.shared_memory_index() is the SHM slot index.
    /// The caller MUST call ReleaseImageFrame() with that index when done.
    bool GetLatestImageFrame(int32_t camera_id, camaramodule::FrameInfo& out) {
        grpc::ClientContext ctx;
        camaramodule::FrameRequest req;
        req.set_camera_id(camera_id);
        grpc::Status s = stub_->GetLatestImageFrame(&ctx, req, &out);
        return check(s, "GetLatestImageFrame");
    }

    bool ReleaseImageFrame(int32_t shm_index) {
        grpc::ClientContext ctx;
        camaramodule::ReleaseRequest req;
        camaramodule::CommandStatus  resp;
        req.set_shared_memory_index(shm_index);
        grpc::Status s = stub_->ReleaseImageFrame(&ctx, req, &resp);
        return check(s, "ReleaseImageFrame") && resp.success();
    }

    // -- Disk save -------------------------------------------------------------

    bool TriggerDiskSave(int32_t camera_id = -1) {
        grpc::ClientContext ctx;
        camaramodule::CameraRequest req;
        camaramodule::CommandStatus resp;
        req.set_camera_id(camera_id);
        grpc::Status s = stub_->TriggerDiskSave(&ctx, req, &resp);
        return check(s, "TriggerDiskSave") && resp.success();
    }

    bool SetSaveDirectory(const std::string& path) {
        grpc::ClientContext ctx;
        camaramodule::SaveDirectoryRequest req;
        camaramodule::CommandStatus        resp;
        req.set_path(path);
        grpc::Status s = stub_->SetSaveDirectory(&ctx, req, &resp);
        return check(s, "SetSaveDirectory") && resp.success();
    }

private:
    static bool check(const grpc::Status& s, const char* rpc) {
        if (s.ok()) return true;
        std::cerr << "[GrpcClient] " << rpc << " error "
                  << s.error_code() << ": " << s.error_message() << "\n";
        return false;
    }

    std::shared_ptr<grpc::Channel>                          channel_;
    std::unique_ptr<camaramodule::CameraControl::Stub>      stub_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Frame statistics helper
// ─────────────────────────────────────────────────────────────────────────────

struct FrameStats {
    uint8_t  min_val{255};
    uint8_t  max_val{0};
    double   mean_val{0.0};
    int32_t  width{0};
    int32_t  height{0};
    int32_t  camera_id{-1};
    int64_t  timestamp_ms{0};
};

static FrameStats ComputeStats(const uint8_t* pixels, int32_t w, int32_t h,
                               int32_t camera_id, int64_t ts)
{
    FrameStats s;
    s.width       = w;
    s.height      = h;
    s.camera_id   = camera_id;
    s.timestamp_ms = ts;

    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (n == 0) return s;

    uint64_t sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const uint8_t v = pixels[i];
        if (v < s.min_val) s.min_val = v;
        if (v > s.max_val) s.max_val = v;
        sum += v;
    }
    s.mean_val = static_cast<double>(sum) / static_cast<double>(n);
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    const std::string address     = (argc > 1) ? argv[1] : "localhost:50051";
    const int32_t     camera_id   = (argc > 2) ? std::stoi(argv[2]) : 0;
    const int         frame_count = (argc > 3) ? std::stoi(argv[3]) : 50;

    std::cout << "GigE Camera Analyzer Example\n"
              << "  server   : " << address     << "\n"
              << "  camera   : " << camera_id   << "\n"
              << "  frames   : " << frame_count << "\n\n";

    // ── 1. Connect to gRPC server ────────────────────────────────────────────
    GrpcClient client(address);

    camaramodule::SystemState state;
    if (!client.GetSystemState(state)) {
        std::cerr << "Cannot reach server at " << address << ". Exiting.\n";
        return 1;
    }
    std::cout << "System state  : " << state.status() << "\n"
              << "Cameras       : " << state.connected_cameras() << "\n\n";

    // ── 2. Print camera info ─────────────────────────────────────────────────
    camaramodule::CameraState cam_info;
    if (client.GetCameraInfo(camera_id, cam_info)) {
        std::cout << "Camera " << camera_id << " info:\n"
                  << "  Model       : " << cam_info.model_name()  << "\n"
                  << "  Serial      : " << cam_info.serial()       << "\n"
                  << "  IP          : " << cam_info.ip_address()   << "\n"
                  << "  ROI         : " << cam_info.width()        << "x"
                                        << cam_info.height()       << "  @("
                                        << cam_info.offset_x()     << ","
                                        << cam_info.offset_y()     << ")\n"
                  << "  Binning     : " << cam_info.binning_h()    << "x"
                                        << cam_info.binning_v()    << "\n"
                  << "  Exposure    : " << cam_info.exposure_us()  << " us\n"
                  << "  Gain        : " << cam_info.gain_db()      << " dB\n"
                  << "  Gamma       : " << cam_info.gamma()        << "\n"
                  << "  Frame rate  : " << cam_info.frame_rate()   << " fps\n"
                  << "  Acquiring   : " << (cam_info.acquiring() ? "yes" : "no") << "\n\n";
    }

    // ── 3. (Optional) adjust parameters before grabbing ─────────────────────
    // Uncomment to set parameters, e.g.:
    //   client.SetFloatParameter("ExposureTime", 10000.0f, camera_id); // 10 ms
    //   client.SetFloatParameter("Gain",          5.0f,    camera_id);
    //   client.SetIntParameter("Width",           1280,    camera_id);
    //   client.SetIntParameter("Height",          1024,    camera_id);

    // ── 4. Start acquisition if not already running ──────────────────────────
    const bool was_idle = (state.status() == "IDLE" || state.status() == "ERROR");
    if (was_idle) {
        std::cout << "Starting acquisition on camera " << camera_id << "...\n";
        if (!client.StartAcquisition(camera_id)) {
            std::cerr << "Failed to start acquisition. Exiting.\n";
            return 1;
        }
    }

    // ── 5. Open SHM (read-only, no admin required) ───────────────────────────
    ShmReader shm;
    if (!shm.open()) {
        std::cerr << "Could not open shared memory. Is GigECameraModule running?\n";
        if (was_idle) client.StopAcquisition(camera_id);
        return 1;
    }

    // ── 6. Frame grab loop ───────────────────────────────────────────────────
    std::cout << "\nGrabbing " << frame_count << " frames...\n";
    std::cout << "  Frame  Cam  WxH            Min  Max  Mean\n";
    std::cout << "  -----  ---  -------------  ---  ---  --------\n";

    int grabbed  = 0;
    int failures = 0;

    for (int i = 0; i < frame_count; ++i) {
        camaramodule::FrameInfo frame;

        // GetLatestImageFrame pins the buffer — must release when done
        if (!client.GetLatestImageFrame(camera_id, frame)) {
            ++failures;
            // Brief wait before retrying (camera may not have a new frame yet)
            Sleep(10);
            continue;
        }

        const int32_t idx = frame.shared_memory_index();
        const int32_t w   = frame.width();
        const int32_t h   = frame.height();

        // Read pixels directly from shared memory
        const uint8_t* pixels = shm.buffer_ptr(idx);
        FrameStats stats = ComputeStats(pixels, w, h, frame.camera_id(), frame.timestamp());

        // Release buffer so the producer can reuse it
        client.ReleaseImageFrame(idx);

        ++grabbed;
        std::printf("  %5d  %3d  %5dx%-6d  %3u  %3u  %8.2f\n",
                    grabbed,
                    stats.camera_id,
                    stats.width,
                    stats.height,
                    stats.min_val,
                    stats.max_val,
                    stats.mean_val);

        // ── Your processing goes here ────────────────────────────────────────
        //
        //   pixels is a (w * h) array of uint8 Mono8 values.
        //   Copy it out before calling ReleaseImageFrame if you need it later.
        //
        //   Example: copy to an OpenCV Mat
        //     cv::Mat mat(h, w, CV_8UC1, const_cast<uint8_t*>(pixels));
        //     cv::Mat owned = mat.clone();  // own copy after release
        //   Then process 'owned' after ReleaseImageFrame.
        //
        // ────────────────────────────────────────────────────────────────────
    }

    std::cout << "\nDone. Grabbed " << grabbed << " frames, " << failures << " failures.\n";

    // ── 7. Stop acquisition if we started it ────────────────────────────────
    if (was_idle) {
        std::cout << "Stopping acquisition on camera " << camera_id << "...\n";
        client.StopAcquisition(camera_id);
    }

    return 0;
}
