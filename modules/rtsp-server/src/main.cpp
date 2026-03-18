#include <windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "FrameGrabber.h"
#include "H264Encoder.h"
#include "RtspServer.h"

static constexpr int DEFAULT_FPS      = 30;
static constexpr int DEFAULT_BITRATE  = 4000;   // kbps
static constexpr int DEFAULT_RTSP_PORT = 8554;

static std::atomic<bool> g_shutdown{false};

static BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        std::cout << "\n[Main] Shutdown signal received.\n";
        g_shutdown.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Args: grpc_addr  camera_id  rtsp_port  fps  bitrate_kbps
    const std::string grpc_addr   = (argc > 1) ? argv[1] : "localhost:50051";
    const int32_t     camera_id   = (argc > 2) ? std::stoi(argv[2]) : 0;
    const uint16_t    rtsp_port   = (argc > 3) ? static_cast<uint16_t>(std::stoi(argv[3])) : DEFAULT_RTSP_PORT;
    const int         fps         = (argc > 4) ? std::stoi(argv[4]) : DEFAULT_FPS;
    const int         bitrate     = (argc > 5) ? std::stoi(argv[5]) : DEFAULT_BITRATE;

    std::cout << "=== GigE RTSP Server ===\n"
              << "  GigECameraModule : " << grpc_addr  << "\n"
              << "  Camera ID        : " << camera_id  << "\n"
              << "  RTSP port        : " << rtsp_port  << "\n"
              << "  FPS              : " << fps        << "\n"
              << "  Bitrate          : " << bitrate    << " kbps\n\n";

    // ── 1. Start frame grabber ────────────────────────────────────────────────
    FrameGrabber grabber(grpc_addr, camera_id);
    if (!grabber.Start()) {
        std::cerr << "[Main] FATAL: FrameGrabber failed to start.\n";
        return 1;
    }

    std::cout << "[Main] Waiting for first frame...\n";
    if (!grabber.WaitFirstFrame(8000)) {
        std::cerr << "[Main] FATAL: No frame received within 8 seconds. "
                     "Is GigECameraModule running and acquisition started?\n";
        grabber.Stop();
        return 1;
    }

    const int w = grabber.Width();
    const int h = grabber.Height();
    std::cout << "[Main] First frame received: " << w << "x" << h << "\n";

    // ── 2. Initialise H.264 encoder ───────────────────────────────────────────
    H264Encoder encoder;
    if (!encoder.Init(w, h, fps, bitrate)) {
        std::cerr << "[Main] FATAL: H264Encoder init failed. "
                     "Is FFmpeg built with libx264 support? "
                     "(vcpkg install ffmpeg[x264] --triplet x64-windows)\n";
        grabber.Stop();
        return 1;
    }

    // ── 3. Start RTSP server (blocks until shutdown) ──────────────────────────
    RtspServer server;

    std::thread server_thread([&] {
        server.Start(rtsp_port, grabber, encoder, fps);
    });

    // Wait for Ctrl+C.
    while (!g_shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── 4. Orderly teardown ───────────────────────────────────────────────────
    std::cout << "[Main] Shutting down...\n";
    server.Stop();
    if (server_thread.joinable()) server_thread.join();
    grabber.Stop();

    std::cout << "[Main] Clean exit.\n";
    return 0;
}
