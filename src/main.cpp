#include <windows.h>

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

#include "SharedMemoryManager.h"
#include "SpinnakerCameraManager.h"
#include "GrpcServer.h"

// ─────────────────────────────────────────────────────────────────────────────
// Defaults
// ─────────────────────────────────────────────────────────────────────────────

// Fallback dimensions used only when no cameras are detected at startup.
// The SHM allocation is otherwise sized to the maximum sensor resolution
// found across all connected cameras (see step 2 below).
static constexpr int32_t FALLBACK_WIDTH    = 4096;
static constexpr int32_t FALLBACK_HEIGHT   = 4096;
static constexpr int32_t DEFAULT_CHANNELS  = 3;      // RGB8 (debayered)

static constexpr const char* DEFAULT_GRPC_ADDR = "0.0.0.0:50051";
static constexpr const char* DEFAULT_SAVE_DIR  = ".";

// ─────────────────────────────────────────────────────────────────────────────
// Graceful-shutdown flag set by the Windows console-control handler
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_shutdown{false};

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT) {
        std::cout << "\n[Main] Shutdown signal received – stopping...\n";
        g_shutdown.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    const char* grpc_addr = (argc > 1) ? argv[1] : DEFAULT_GRPC_ADDR;
    const char* save_dir  = (argc > 2) ? argv[2] : DEFAULT_SAVE_DIR;

    std::cout << "=== Teledyne GigE Camera Module ===\n"
              << "gRPC address : " << grpc_addr << '\n'
              << "Save dir     : " << save_dir  << '\n';

    // ── 1. Spinnaker camera manager ───────────────────────────────────────────
    // Enumerate cameras and call Init() on each so their GenICam NodeMaps are
    // accessible.  SHM is not yet allocated — the constructor only stores the
    // reference; no SHM methods are called until after step 3.
    SharedMemoryManager    shm;
    SpinnakerCameraManager cam_mgr(shm);

    if (!cam_mgr.Initialize(save_dir)) {
        std::cerr << "[Main] FATAL: Failed to initialise Spinnaker.\n";
        return 1;
    }

    const int32_t cam_count = cam_mgr.GetConnectedCameraCount();
    std::cout << "[Main] " << cam_count << " camera(s) found.\n";

    // ── 2. Determine SHM buffer dimensions from actual sensor sizes ───────────
    // Each slot in the shared pool must be large enough to hold the largest
    // frame that any connected camera can produce (at full sensor resolution,
    // before any ROI or binning is applied).
    int32_t shm_width  = 0;
    int32_t shm_height = 0;
    cam_mgr.GetMaxImageDimensions(shm_width, shm_height,
                                  FALLBACK_WIDTH, FALLBACK_HEIGHT);

    std::cout << "[Main] SHM buffer slot size: " << shm_width
              << "x" << shm_height << "x" << DEFAULT_CHANNELS
              << " (" << (static_cast<int64_t>(shm_width) * shm_height * DEFAULT_CHANNELS / 1024 / 1024)
              << " MB per slot, "
              << (static_cast<int64_t>(shm_width) * shm_height * DEFAULT_CHANNELS * POOL_SIZE / 1024 / 1024)
              << " MB total pool)\n";

    // ── 3. Shared memory ──────────────────────────────────────────────────────
    if (!shm.Initialize(shm_width, shm_height, DEFAULT_CHANNELS)) {
        std::cerr << "[Main] FATAL: Failed to create shared memory.\n"
                  << "       The process must be run as Administrator to create\n"
                  << "       objects in the Global\\ namespace.\n";
        return 1;
    }

    // SetNumCameras was a no-op during cam_mgr.Initialize() because SHM wasn't
    // ready yet — call it now.
    shm.SetNumCameras(cam_count);

    // ── 4. gRPC server (run on background thread so we can poll g_shutdown) ───
    GrpcServer grpc_server(cam_mgr, shm);

    std::thread grpc_thread([&] {
        try {
            grpc_server.Start(grpc_addr);
        } catch (const std::exception& ex) {
            std::cerr << "[Main] gRPC server error: " << ex.what() << '\n';
            g_shutdown.store(true, std::memory_order_release);
        }
    });

    // ── 5. Main loop – wait for shutdown signal ────────────────────────────────
    while (!g_shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── 6. Orderly teardown ───────────────────────────────────────────────────
    std::cout << "[Main] Shutting down...\n";

    grpc_server.Shutdown();
    if (grpc_thread.joinable()) grpc_thread.join();

    cam_mgr.Shutdown();
    shm.Shutdown();

    std::cout << "[Main] Clean exit.\n";
    return 0;
}
