//
// GigEDebugClient — interactive gRPC REPL for testing the camera module.
//
// Usage:
//   GigEDebugClient.exe [server_address]   (default: localhost:50051)
//
// The client also opens the shared memory block directly (read-only, no admin
// required) to inspect the raw pixel data of grabbed frames.
//

#include <windows.h>

#include <grpcpp/grpcpp.h>
#include "camera_service.grpc.pb.h"

// SharedMemoryHeader layout must match the producer.
#include "SharedMemoryManager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static constexpr const char* DEFAULT_ADDR = "localhost:50051";

// ─────────────────────────────────────────────────────────────────────────────
// Shared memory reader (consumer side, read-only, no SeCreateGlobalPrivilege)
// ─────────────────────────────────────────────────────────────────────────────

struct ShmView {
    HANDLE      handle{NULL};
    const void* view{nullptr};

    ShmView() {
        handle = OpenFileMappingA(FILE_MAP_READ, FALSE, SHM_NAME);
        if (!handle) return;
        view = MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0);
    }

    ~ShmView() {
        if (view)   UnmapViewOfFile(view);
        if (handle) CloseHandle(handle);
    }

    bool ok() const { return view != nullptr; }

    const SharedMemoryHeader* header() const {
        return reinterpret_cast<const SharedMemoryHeader*>(view);
    }

    const uint8_t* buffer(int32_t index) const {
        const auto* hdr  = header();
        const auto* base = reinterpret_cast<const uint8_t*>(view) + sizeof(SharedMemoryHeader);
        return base + static_cast<std::size_t>(index) * hdr->single_image_size;
    }
};

static void InspectBuffer(int32_t idx) {
    ShmView shm;
    if (!shm.ok()) {
        std::cout << "  [SHM] Cannot open shared memory (error " << GetLastError()
                  << "). Is the camera module running?\n";
        return;
    }

    const SharedMemoryHeader* hdr = shm.header();
    if (idx < 0 || idx >= hdr->pool_size) {
        std::cout << "  [SHM] Index " << idx << " out of range [0, "
                  << hdr->pool_size << ").\n";
        return;
    }

    const std::size_t  n            = hdr->single_image_size;
    const uint8_t*     buf          = shm.buffer(idx);
    const int64_t      ts_us        = hdr->buffer_timestamp_us[idx];
    const int32_t      buf_channels = hdr->buffer_channels[idx];

    if (n == 0) {
        std::cout << "  [SHM] single_image_size is 0 — header not yet initialised.\n";
        return;
    }

    uint64_t sum  = 0;
    uint8_t  vmin = 255;
    uint8_t  vmax = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sum  += buf[i];
        vmin = std::min(vmin, buf[i]);
        vmax = std::max(vmax, buf[i]);
    }
    const double mean = static_cast<double>(sum) / static_cast<double>(n);

    // Use per-buffer dimensions — these reflect the actual ROI at capture time,
    // which may differ from hdr->image_width/height if the ROI was changed.
    const int32_t w = hdr->buffer_width[idx];
    const int32_t h = hdr->buffer_height[idx];

    std::cout << "  [SHM] buffer[" << idx << "] — "
              << n << " bytes  |  "
              << "min=" << static_cast<int>(vmin)
              << "  max=" << static_cast<int>(vmax)
              << "  mean=" << std::fixed << std::setprecision(1) << mean << '\n';
    if (ts_us > 0)
        std::cout << "  [SHM] timestamp : " << ts_us << " us\n";

    if (w > 0 && h > 0) {
        const int32_t ch = (buf_channels > 0) ? buf_channels : 3;
        const char*   fmt = (ch == 1) ? "Bayer/raw" : (ch == 3 ? "BGR8" : "unknown");
        std::cout << "  [SHM] Pixel sample (5×5 grid across " << w << "×" << h
                  << ", " << fmt << "):\n        ";
        for (int gy = 0; gy < 5; ++gy) {
            for (int gx = 0; gx < 5; ++gx) {
                const int px = static_cast<int>(gx * (w - 1) / 4);
                const int py = static_cast<int>(gy * (h - 1) / 4);
                // For multi-channel images, sample the first channel (B in BGR8).
                std::cout << std::setw(4)
                          << static_cast<int>(buf[(py * w + px) * ch]);
            }
            std::cout << '\n' << "        ";
        }
        std::cout << '\n';
    }
}

static void PrintShmState() {
    ShmView shm;
    if (!shm.ok()) {
        std::cout << "  [SHM] Cannot open shared memory.\n";
        return;
    }
    const SharedMemoryHeader* hdr = shm.header();
    std::cout << "  [SHM] allocated=" << hdr->image_width << "x" << hdr->image_height
              << "x" << hdr->image_channels
              << "  pool=" << hdr->pool_size
              << "  cameras=" << hdr->num_cameras
              << "  global_latest=" << hdr->latest_buffer_index.load()
              << '\n';

    std::cout << "  [SHM] Per-camera latest  : ";
    for (int c = 0; c < hdr->num_cameras && c < MAX_CAMERAS; ++c)
        std::cout << "[cam" << c << "]=" << hdr->latest_buffer_per_camera[c].load() << ' ';
    std::cout << '\n';

    std::cout << "  [SHM] Buffer states (refcount / camera / dims / channels):\n        ";
    for (int i = 0; i < hdr->pool_size; ++i) {
        const int32_t rc  = hdr->reference_counts[i].load();
        const int32_t cam = hdr->buffer_camera_id[i];
        const int32_t bw  = hdr->buffer_width[i];
        const int32_t bh  = hdr->buffer_height[i];
        const int32_t bch = hdr->buffer_channels[i];
        std::cout << "[" << i << "]rc=" << rc << "/c" << cam
                  << "/" << bw << "x" << bh << "x" << bch << "  ";
        if ((i + 1) % 4 == 0) std::cout << "\n        ";
    }
    std::cout << '\n';
}

// ─────────────────────────────────────────────────────────────────────────────
// gRPC client wrapper
// ─────────────────────────────────────────────────────────────────────────────

class DebugClient {
public:
    explicit DebugClient(const std::string& address)
        : stub_(camaramodule::CameraControl::NewStub(
              grpc::CreateChannel(address, grpc::InsecureChannelCredentials()))) {
        std::cout << "[DebugClient] Targeting " << address << '\n';
    }

    // ── Module-level ──────────────────────────────────────────────────────────

    void Health() {
        camaramodule::Empty       req;
        camaramodule::SystemState resp;
        grpc::ClientContext       ctx;
        // Short deadline so we fail fast on an unreachable module.
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
        auto st = stub_->GetSystemState(&ctx, req, &resp);
        if (st.ok()) {
            std::cout << "  UP  —  status=" << resp.status()
                      << "  camera count=" << resp.connected_cameras() << '\n';
        } else {
            std::cout << "  DOWN (code=" << static_cast<int>(st.error_code())
                      << "): " << st.error_message() << '\n';
        }
    }

    void GetSystemState() {
        camaramodule::Empty       req;
        camaramodule::SystemState resp;
        grpc::ClientContext       ctx;
        auto st = stub_->GetSystemState(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        std::cout << "  status       : " << resp.status()            << '\n'
                  << "  camera count : " << resp.connected_cameras() << '\n';
    }

    void StartAcquisition(int32_t camera_id = -1) {
        camaramodule::CameraRequest  req;
        camaramodule::CommandStatus  resp;
        grpc::ClientContext          ctx;
        req.set_camera_id(camera_id);
        auto st = stub_->StartAcquisition(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        PrintStatus(resp);
    }

    void StopAcquisition(int32_t camera_id = -1) {
        camaramodule::CameraRequest  req;
        camaramodule::CommandStatus  resp;
        grpc::ClientContext          ctx;
        req.set_camera_id(camera_id);
        auto st = stub_->StopAcquisition(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        PrintStatus(resp);
    }

    void Restart(int32_t camera_id = -1) {
        std::cout << "  [restart] Stopping...\n";
        StopAcquisition(camera_id);
        std::cout << "  [restart] Starting...\n";
        StartAcquisition(camera_id);
    }

    /// camera_id = -1 means all cameras.
    /// Pass a non-empty sval to set enumeration nodes (ExposureAuto, GainAuto, …).
    void SetParameter(const std::string& name, float fval, int32_t ival,
                      int32_t camera_id = -1, const std::string& sval = {}) {
        camaramodule::ParameterRequest req;
        camaramodule::CommandStatus    resp;
        grpc::ClientContext            ctx;
        req.set_camera_id(camera_id);
        req.set_param_name(name);
        req.set_float_value(fval);
        req.set_int_value(ival);
        req.set_string_value(sval);
        auto st = stub_->SetParameter(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        PrintStatus(resp);
    }

    void TriggerDiskSave(int32_t camera_id = -1) {
        camaramodule::CameraRequest  req;
        camaramodule::CommandStatus  resp;
        grpc::ClientContext          ctx;
        req.set_camera_id(camera_id);
        auto st = stub_->TriggerDiskSave(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        PrintStatus(resp);
    }

    void SetSaveDirectory(const std::string& path) {
        camaramodule::SaveDirectoryRequest req;
        camaramodule::CommandStatus        resp;
        grpc::ClientContext                ctx;
        req.set_path(path);
        auto st = stub_->SetSaveDirectory(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        PrintStatus(resp);
    }

    void GetCameraInfo(int32_t camera_id) {
        camaramodule::CameraRequest req;
        camaramodule::CameraState   resp;
        grpc::ClientContext         ctx;
        req.set_camera_id(camera_id);
        auto st = stub_->GetCameraInfo(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        PrintCameraState(resp);
    }

    /// Queries all cameras and prints a summary table.
    void ListCameras() {
        camaramodule::Empty       sreq;
        camaramodule::SystemState sres;
        grpc::ClientContext       sctx;
        auto st = stub_->GetSystemState(&sctx, sreq, &sres);
        if (!st.ok()) { PrintRpcError(st); return; }

        const int32_t count = sres.connected_cameras();
        if (count == 0) {
            std::cout << "  No cameras connected.\n";
            return;
        }

        for (int32_t i = 0; i < count; ++i) {
            std::cout << "  --- Camera " << i << " -----------------------------------\n";
            GetCameraInfo(i);
        }
    }

    void GrabFrame(int32_t camera_id, bool keep = false) {
        camaramodule::FrameRequest req;
        camaramodule::FrameInfo    resp;
        grpc::ClientContext        ctx;
        req.set_camera_id(camera_id);
        auto st = stub_->GetLatestImageFrame(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }

        const int32_t idx = resp.shared_memory_index();
        std::cout << "  shm_index : " << idx                << '\n'
                  << "  camera_id : " << resp.camera_id()   << '\n'
                  << "  size      : " << resp.width() << "×" << resp.height() << '\n'
                  << "  timestamp : " << resp.timestamp()   << " us\n";

        InspectBuffer(idx);

        if (!keep) {
            ReleaseFrame(idx);
        } else {
            std::cout << "  [kept — remember to 'release " << idx << "']\n";
        }
    }

    void ReleaseFrame(int32_t index) {
        camaramodule::ReleaseRequest req;
        camaramodule::CommandStatus  resp;
        grpc::ClientContext          ctx;
        req.set_shared_memory_index(index);
        auto st = stub_->ReleaseImageFrame(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        PrintStatus(resp);
    }

private:
    static void PrintStatus(const camaramodule::CommandStatus& s) {
        std::cout << "  " << (s.success() ? "OK" : "FAIL")
                  << " — " << s.message() << '\n';
    }

    static void PrintRpcError(const grpc::Status& s) {
        std::cerr << "  [gRPC error " << static_cast<int>(s.error_code())
                  << "] " << s.error_message() << '\n';
    }

    static void PrintCameraState(const camaramodule::CameraState& s) {
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "  model      : " << (s.model_name().empty() ? "(unknown)" : s.model_name()) << '\n'
                  << "  serial     : " << (s.serial().empty()     ? "(unknown)" : s.serial())     << '\n'
                  << "  ip         : " << (s.ip_address().empty() ? "n/a"       : s.ip_address()) << '\n'
                  << "  acquiring  : " << (s.acquiring() ? "yes" : "no")                          << '\n'
                  << "  fps        : " << s.fps()                                                  << '\n'
                  << "  resolution : " << s.width() << "x" << s.height()                          << '\n'
                  << "  ROI offset : " << s.offset_x() << ", " << s.offset_y()                    << '\n'
                  << "  binning    : " << s.binning_h() << "x" << s.binning_v()                   << '\n'
                  << "  exposure   : " << s.exposure_us() << " us"
                  << "  [auto: " << (s.exposure_auto().empty() ? "?" : s.exposure_auto()) << "]\n"
                  << "  gain       : " << s.gain_db()     << " dB"
                  << "  [auto: " << (s.gain_auto().empty()     ? "?" : s.gain_auto())     << "]\n"
                  << "  gamma      : " << s.gamma()       << '\n'
                  << "  black lvl  : " << s.black_level() << '\n'
                  << "  frame rate : " << (s.frame_rate() > 0.0f
                                            ? std::to_string(s.frame_rate()) + " fps"
                                            : "n/a (node unavailable)") << '\n';
    }

    std::unique_ptr<camaramodule::CameraControl::Stub> stub_;
};

// ─────────────────────────────────────────────────────────────────────────────
// REPL
// ─────────────────────────────────────────────────────────────────────────────

static void PrintHelp() {
    std::cout << R"(
Module commands:
  health                        Ping the module (UP/DOWN + current status)
  state                         Detailed system state (camera count, FPS)
  start  [cam_id]               Start acquisition  (-1 or omit = all cameras)
  stop   [cam_id]               Stop acquisition   (-1 or omit = all cameras)
  restart [cam_id]              Stop then start    (-1 or omit = all cameras)

Camera info:
  cameras                       List all cameras with full state
  info <cam_id>                 Show state for one camera (model, IP, ROI, …)

Parameter control:
  set <name> <float> <int> [str]           Set GenICam node on ALL cameras
  set <cam_id> <name> <f> <i> [str]       Set GenICam node on one camera
    Float nodes:   set ExposureTime 5000.0 0
                   set 0 Gain 10.0 0
    Integer nodes: set -1 Width 0 1920
    Enum nodes:    set ExposureAuto 0 0 Continuous
                   set GainAuto 0 0 Off
                   set 0 ExposureAuto 0 0 Once
    Channel order: set 0 ChannelOrder 0 0 BGR   (default: red=red, blue=blue)
                   set 1 ChannelOrder 0 0 RGB   (skip R↔B swap for camera 1)
    Debayer mode:  set 0 DebayerMode  0 0 Off   (raw Bayer in SHM, saves .raw)
                   set 0 DebayerMode  0 0 On    (debayer → BGR8, default)

Disk save:
  save [cam_id]                 Flag next JPEG frame for disk write (-1 or omit = any camera)
  savedir <path>                Change save directory at runtime

Frame inspection:
  grab [cam_id] [keep]          Grab frame + SHM inspect + auto-release
                                  cam_id : 0, 1, … or -1 for any  (default -1)
                                  keep   : skip auto-release
  release <index>               Release a held frame buffer

SHM diagnostics:
  shm                           Dump full SHM pool state
  inspect <index>               Pixel stats on a buffer (no gRPC needed)

  help                          Show this help
  quit / exit                   Exit
)" << '\n';
}

int main(int argc, char* argv[]) {
    // Switch the console to UTF-8 so any Unicode that does slip through renders
    // correctly.  All user-visible strings below use plain ASCII intentionally.
    SetConsoleOutputCP(CP_UTF8);

    const std::string addr = (argc > 1) ? argv[1] : DEFAULT_ADDR;

    DebugClient client(addr);
    PrintHelp();

    std::string line;
    while (true) {
        std::cout << "camera> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        std::istringstream ss(line);
        std::string        cmd;
        ss >> cmd;

        if (cmd.empty())                    continue;
        if (cmd == "quit" || cmd == "exit") break;
        if (cmd == "help")                  { PrintHelp();                continue; }
        if (cmd == "health")                { client.Health();            continue; }
        if (cmd == "state")                 { client.GetSystemState();    continue; }
        if (cmd == "save") {
            int32_t cam_id = -1;
            ss >> cam_id;  // optional; stays -1 (any camera) if not provided
            client.TriggerDiskSave(cam_id);
            continue;
        }
        if (cmd == "shm")                   { PrintShmState();            continue; }
        if (cmd == "cameras")               { client.ListCameras();       continue; }

        if (cmd == "start" || cmd == "stop" || cmd == "restart") {
            int32_t cam_id = -1;
            ss >> cam_id;  // optional; stays -1 if not provided
            if      (cmd == "start")   client.StartAcquisition(cam_id);
            else if (cmd == "stop")    client.StopAcquisition(cam_id);
            else                       client.Restart(cam_id);
            continue;
        }

        if (cmd == "info") {
            int32_t cam_id = -1;
            if (!(ss >> cam_id)) {
                std::cerr << "  Usage: info <cam_id>\n";
                continue;
            }
            client.GetCameraInfo(cam_id);
            continue;
        }

        if (cmd == "savedir") {
            std::string path;
            if (!(ss >> path)) {
                std::cerr << "  Usage: savedir <path>\n";
                continue;
            }
            client.SetSaveDirectory(path);
            continue;
        }

        if (cmd == "set") {
            // Syntax:
            //   set <name> <float> <int> [string]           — all cameras
            //   set <cam_id> <name> <float> <int> [string]  — one camera
            // string is optional: supply it for enumeration nodes
            //   e.g.  set ExposureAuto 0 0 Continuous
            //         set 0 GainAuto 0 0 Off
            std::string token;
            if (!(ss >> token)) {
                std::cerr << "  Usage: set [cam_id] <node_name> <float> <int> [string]\n";
                continue;
            }

            int32_t     camera_id = -1;
            std::string name;

            // If the first token parses as an integer it is the camera_id.
            try {
                camera_id = std::stoi(token);
                if (!(ss >> name)) {
                    std::cerr << "  Usage: set [cam_id] <node_name> <float> <int> [string]\n";
                    continue;
                }
            } catch (...) {
                // First token was a node name — apply to all cameras.
                name = token;
            }

            float   fval = 0.0f;
            int32_t ival = 0;
            if (!(ss >> fval >> ival)) {
                std::cerr << "  Usage: set [cam_id] <node_name> <float> <int> [string]\n";
                continue;
            }

            // Optional string_value for enumeration nodes.
            std::string sval;
            ss >> sval;

            client.SetParameter(name, fval, ival, camera_id, sval);
            continue;
        }

        if (cmd == "grab") {
            int32_t     cam_id = -1;
            std::string extra;
            ss >> cam_id;
            ss >> extra;
            const bool keep = (extra == "keep");
            client.GrabFrame(cam_id, keep);
            continue;
        }

        if (cmd == "release") {
            int32_t idx = -1;
            if (!(ss >> idx)) {
                std::cerr << "  Usage: release <shm_index>\n";
                continue;
            }
            client.ReleaseFrame(idx);
            continue;
        }

        if (cmd == "inspect") {
            int32_t idx = -1;
            if (!(ss >> idx)) {
                std::cerr << "  Usage: inspect <shm_index>\n";
                continue;
            }
            InspectBuffer(idx);
            continue;
        }

        std::cerr << "  Unknown command '" << cmd << "'. Type 'help'.\n";
    }

    return 0;
}
