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

// Pre-allocated shared memory dimensions.  These must cover the largest image
// the connected cameras will produce.  Adjust before building if your sensors
// are larger than 1920×1080 Mono8.
static constexpr int32_t DEFAULT_WIDTH    = 1920;
static constexpr int32_t DEFAULT_HEIGHT   = 1080;
static constexpr int32_t DEFAULT_CHANNELS = 1;      // Mono8

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
        return TRUE; // suppress default handler (which would terminate immediately)
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

    // ── 1. Shared memory ──────────────────────────────────────────────────────
    SharedMemoryManager shm;
    if (!shm.Initialize(DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_CHANNELS)) {
        std::cerr << "[Main] FATAL: Failed to create shared memory.\n"
                  << "       The process must be run as Administrator to create\n"
                  << "       objects in the Global\\ namespace.\n";
        return 1;
    }

    // ── 2. Spinnaker camera manager ───────────────────────────────────────────
    SpinnakerCameraManager cam_mgr(shm);
    if (!cam_mgr.Initialize(save_dir)) {
        std::cerr << "[Main] FATAL: Failed to initialise Spinnaker.\n";
        return 1;
    }

    std::cout << "[Main] " << cam_mgr.GetConnectedCameraCount()
              << " camera(s) ready.\n";

    // ── 3. gRPC server (run on background thread so we can poll g_shutdown) ───
    GrpcServer grpc_server(cam_mgr, shm);

    std::thread grpc_thread([&] {
        try {
            grpc_server.Start(grpc_addr);
        } catch (const std::exception& ex) {
            std::cerr << "[Main] gRPC server error: " << ex.what() << '\n';
            g_shutdown.store(true, std::memory_order_release);
        }
    });

    // ── 4. Main loop – wait for shutdown signal ────────────────────────────────
    while (!g_shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── 5. Orderly teardown ───────────────────────────────────────────────────
    std::cout << "[Main] Shutting down...\n";

    grpc_server.Shutdown();
    if (grpc_thread.joinable()) grpc_thread.join();

    cam_mgr.Shutdown();
    shm.Shutdown();

    std::cout << "[Main] Clean exit.\n";
    return 0;
}
