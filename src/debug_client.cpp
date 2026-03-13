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

    const std::size_t  n   = hdr->single_image_size;
    const uint8_t*     buf = shm.buffer(idx);

    if (n == 0) {
        std::cout << "  [SHM] single_image_size is 0 — header not yet initialised.\n";
        return;
    }

    // Basic statistics
    uint64_t sum   = 0;
    uint8_t  vmin  = 255;
    uint8_t  vmax  = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sum  += buf[i];
        vmin = std::min(vmin, buf[i]);
        vmax = std::max(vmax, buf[i]);
    }
    const double mean = static_cast<double>(sum) / static_cast<double>(n);

    // Sample a 5×5 grid of pixels for a quick visual thumbnail
    const int32_t w = hdr->image_width;
    const int32_t h = hdr->image_height;

    std::cout << "  [SHM] buffer[" << idx << "] — "
              << n << " bytes  |  "
              << "min=" << static_cast<int>(vmin)
              << "  max=" << static_cast<int>(vmax)
              << "  mean=" << std::fixed << std::setprecision(1) << mean << '\n';

    if (w > 0 && h > 0) {
        std::cout << "  [SHM] Pixel sample (5×5 grid across " << w << "×" << h << "):\n        ";
        for (int gy = 0; gy < 5; ++gy) {
            for (int gx = 0; gx < 5; ++gx) {
                const int px = static_cast<int>(gx * (w - 1) / 4);
                const int py = static_cast<int>(gy * (h - 1) / 4);
                std::cout << std::setw(4) << static_cast<int>(buf[py * w + px]);
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
    std::cout << "  [SHM] " << hdr->image_width << "×" << hdr->image_height
              << "×" << hdr->image_channels
              << "  pool=" << hdr->pool_size
              << "  cameras=" << hdr->num_cameras
              << "  global_latest=" << hdr->latest_buffer_index.load()
              << '\n';

    std::cout << "  [SHM] Per-camera latest  : ";
    for (int c = 0; c < hdr->num_cameras && c < MAX_CAMERAS; ++c)
        std::cout << "[cam" << c << "]=" << hdr->latest_buffer_per_camera[c].load() << ' ';
    std::cout << '\n';

    std::cout << "  [SHM] Buffer states (refcount / camera):\n        ";
    for (int i = 0; i < hdr->pool_size; ++i) {
        const int32_t rc  = hdr->reference_counts[i].load();
        const int32_t cam = hdr->buffer_camera_id[i];
        std::cout << "[" << i << "]rc=" << rc << "/c" << cam << "  ";
        if ((i + 1) % 5 == 0) std::cout << "\n        ";
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

    // ── RPCs ──────────────────────────────────────────────────────────────────

    void GetSystemState() {
        camaramodule::Empty      req;
        camaramodule::SystemState resp;
        grpc::ClientContext       ctx;
        auto st = stub_->GetSystemState(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        std::cout << "  status   : " << resp.status()            << '\n'
                  << "  cameras  : " << resp.connected_cameras() << '\n'
                  << "  fps      : " << resp.current_fps()       << '\n';
    }

    void StartAcquisition() {
        SimpleCommand([this](auto& ctx, auto& req, auto& resp) {
            return stub_->StartAcquisition(&ctx, req, &resp);
        });
    }

    void StopAcquisition() {
        SimpleCommand([this](auto& ctx, auto& req, auto& resp) {
            return stub_->StopAcquisition(&ctx, req, &resp);
        });
    }

    void SetParameter(const std::string& name, float fval, int32_t ival) {
        camaramodule::ParameterRequest req;
        req.set_param_name(name);
        req.set_float_value(fval);
        req.set_int_value(ival);
        camaramodule::CommandStatus resp;
        grpc::ClientContext         ctx;
        auto st = stub_->SetParameter(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        PrintStatus(resp);
    }

    void TriggerDiskSave() {
        SimpleCommand([this](auto& ctx, auto& req, auto& resp) {
            return stub_->TriggerDiskSave(&ctx, req, &resp);
        });
    }

    /// Grabs the latest frame from @p camera_id (use -1 for any camera),
    /// prints metadata, inspects the SHM buffer, then releases.
    void GrabFrame(int32_t camera_id, bool keep = false) {
        camaramodule::FrameRequest req;
        req.set_camera_id(camera_id);
        camaramodule::FrameInfo resp;
        grpc::ClientContext     ctx;
        auto st = stub_->GetLatestImageFrame(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }

        const int32_t idx = resp.shared_memory_index();
        std::cout << "  shm_index : " << idx                << '\n'
                  << "  camera_id : " << resp.camera_id()   << '\n'
                  << "  size      : " << resp.width() << "×" << resp.height() << '\n'
                  << "  timestamp : " << resp.timestamp()   << " ms\n";

        InspectBuffer(idx);

        if (!keep) {
            ReleaseFrame(idx);
        } else {
            std::cout << "  [kept — remember to 'release " << idx << "']\n";
        }
    }

    void ReleaseFrame(int32_t index) {
        camaramodule::ReleaseRequest req;
        req.set_shared_memory_index(index);
        camaramodule::CommandStatus resp;
        grpc::ClientContext         ctx;
        auto st = stub_->ReleaseImageFrame(&ctx, req, &resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        PrintStatus(resp);
    }

private:
    // Helper: call any RPC that takes Empty and returns CommandStatus.
    template<typename Fn>
    void SimpleCommand(Fn fn) {
        camaramodule::Empty         req;
        camaramodule::CommandStatus resp;
        grpc::ClientContext         ctx;
        auto st = fn(ctx, req, resp);
        if (!st.ok()) { PrintRpcError(st); return; }
        PrintStatus(resp);
    }

    static void PrintStatus(const camaramodule::CommandStatus& s) {
        std::cout << "  " << (s.success() ? "OK" : "FAIL")
                  << " — " << s.message() << '\n';
    }

    static void PrintRpcError(const grpc::Status& s) {
        std::cerr << "  [gRPC error " << static_cast<int>(s.error_code())
                  << "] " << s.error_message() << '\n';
    }

    std::unique_ptr<camaramodule::CameraControl::Stub> stub_;
};

// ─────────────────────────────────────────────────────────────────────────────
// REPL
// ─────────────────────────────────────────────────────────────────────────────

static void PrintHelp() {
    std::cout << R"(
Commands:
  state                         GetSystemState
  start                         StartAcquisition
  stop                          StopAcquisition
  set <node> <float> <int>      SetParameter
                                  e.g.  set ExposureTime 5000.0 0
                                        set Gain 10.0 0
                                        set Width 0 1920
  save                          TriggerDiskSave (flags next frame for disk write)

  grab [camera_id] [keep]       GetLatestImageFrame + SHM inspect + auto-release
                                  camera_id : 0, 1, … or -1 for any  (default -1)
                                  keep      : pass 'keep' to skip auto-release
                                  e.g.  grab        — any camera, auto-release
                                        grab 0      — camera 0, auto-release
                                        grab 1 keep — camera 1, hold buffer
  release <index>               ReleaseImageFrame (use after 'grab … keep')

  shm                           Print shared memory pool state directly
  inspect <index>               Print pixel stats for a buffer without going via gRPC

  help                          Show this help
  quit / exit                   Exit
)" << '\n';
}

int main(int argc, char* argv[]) {
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
        if (cmd == "state")                 { client.GetSystemState();    continue; }
        if (cmd == "start")                 { client.StartAcquisition();  continue; }
        if (cmd == "stop")                  { client.StopAcquisition();   continue; }
        if (cmd == "save")                  { client.TriggerDiskSave();   continue; }
        if (cmd == "shm")                   { PrintShmState();            continue; }

        if (cmd == "set") {
            std::string name;
            float       fval = 0.0f;
            int32_t     ival = 0;
            if (!(ss >> name >> fval >> ival)) {
                std::cerr << "  Usage: set <node_name> <float_value> <int_value>\n";
                continue;
            }
            client.SetParameter(name, fval, ival);
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
