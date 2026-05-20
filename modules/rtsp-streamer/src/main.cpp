// GigERTSPStreamer — H.264 video streaming from GigECameraModule
//
// Usage: GigERTSPStreamer.exe [options]
//
// Options:
//   --grpc <addr>          GigECameraModule gRPC address (default: localhost:50051)
//   --cameras <ids>        Comma-separated camera IDs   (default: 0)
//   --mode server|push     Streaming mode               (default: server)
//   --port <n>             RTSP listen port [server]    (default: 8554)
//   --target <host:port>   RTP push destination [push]  (default: 127.0.0.1:5004)
//   --fps <n>              Target frame rate             (default: 30)
//   --bitrate <kbps>       H.264 bitrate                (default: 4000)
//
// Server mode — connect with any RTSP player:
//   rtsp://host:8554/cam0    (camera 0)
//   rtsp://host:8554/cam1    (camera 1)
//
// Push mode — raw RTP/UDP, no RTSP negotiation:
//   Camera 0 → target_host:port
//   Camera 1 → target_host:port+2
//   Receive:  ffplay rtp://0.0.0.0:<port>   |  vlc rtp://@:<port>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "FrameGrabber.h"
#include "H264Encoder.h"
#include "RtspServer.h"
#include "RtpPusher.h"

static std::atomic<bool> g_shutdown{false};

static BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        std::cout << "\n[Main] Shutdown signal received.\n";
        g_shutdown.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration + argument parsing
// ─────────────────────────────────────────────────────────────────────────────

struct Config {
    std::string          grpc_addr  = "localhost:50051";
    std::vector<int32_t> cameras    = {0};
    std::string          mode       = "server";    // "server" | "push"
    uint16_t             rtsp_port  = 8554;
    std::string          push_host  = "127.0.0.1";
    uint16_t             push_port  = 5004;
    int                  fps        = 30;
    int                  bitrate    = 4000;  // kbps
};

static std::string NextArg(int argc, char* argv[], int& i) {
    if (i + 1 >= argc) {
        std::cerr << "Missing value for " << argv[i] << "\n";
        std::exit(1);
    }
    return argv[++i];
}

static std::vector<int32_t> ParseCameraIds(const std::string& s) {
    std::vector<int32_t> ids;
    std::istringstream   ss(s);
    std::string          tok;
    while (std::getline(ss, tok, ',')) {
        try { ids.push_back(std::stoi(tok)); }
        catch (...) {
            std::cerr << "Invalid camera id: '" << tok << "'\n";
            std::exit(1);
        }
    }
    return ids.empty() ? std::vector<int32_t>{0} : ids;
}

static std::pair<std::string, uint16_t> ParseHostPort(const std::string& s) {
    auto pos = s.rfind(':');
    if (pos == std::string::npos || pos == 0) {
        std::cerr << "Expected host:port, got: '" << s << "'\n";
        std::exit(1);
    }
    return {s.substr(0, pos), static_cast<uint16_t>(std::stoi(s.substr(pos + 1)))};
}

static Config ParseArgs(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--grpc")    cfg.grpc_addr = NextArg(argc, argv, i);
        else if (arg == "--cameras") cfg.cameras   = ParseCameraIds(NextArg(argc, argv, i));
        else if (arg == "--mode")    cfg.mode      = NextArg(argc, argv, i);
        else if (arg == "--port")    cfg.rtsp_port = static_cast<uint16_t>(std::stoi(NextArg(argc, argv, i)));
        else if (arg == "--fps")     cfg.fps       = std::stoi(NextArg(argc, argv, i));
        else if (arg == "--bitrate") cfg.bitrate   = std::stoi(NextArg(argc, argv, i));
        else if (arg == "--target") {
            auto [h, p]  = ParseHostPort(NextArg(argc, argv, i));
            cfg.push_host = h;
            cfg.push_port = p;
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << R"(
GigERTSPStreamer — H.264 video streaming from GigECameraModule

Usage: GigERTSPStreamer.exe [options]

  --grpc <addr>          GigECameraModule gRPC address  (default: localhost:50051)
  --cameras <ids>        Comma-separated camera IDs     (default: 0)
                           examples: --cameras 0    --cameras 0,1,2
  --mode server|push     Streaming mode                 (default: server)
  --port <n>             RTSP listen port [server mode] (default: 8554)
  --target <host:port>   RTP push destination [push]    (default: 127.0.0.1:5004)
  --fps <n>              Target frame rate               (default: 30)
  --bitrate <kbps>       H.264 bitrate                  (default: 4000)

Server mode:
  Camera 0 → rtsp://host:8554/cam0
  Camera 1 → rtsp://host:8554/cam1
  Open with VLC, ffplay, or any RTSP-capable player.

Push mode (raw RTP/UDP — no RTSP handshake):
  Camera 0 → target_host:port
  Camera 1 → target_host:port+2
  Receive:  ffplay  rtp://0.0.0.0:<port>
            vlc     rtp://@:<port>
)" << "\n";
            std::exit(0);
        }
        else {
            std::cerr << "Unknown option: " << arg << "  (use --help)\n";
            std::exit(1);
        }
    }

    if (cfg.mode != "server" && cfg.mode != "push") {
        std::cerr << "Unknown mode '" << cfg.mode << "' — use 'server' or 'push'.\n";
        std::exit(1);
    }
    return cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-camera streaming context
// ─────────────────────────────────────────────────────────────────────────────

struct CameraSlot {
    int32_t                          camera_id{-1};
    std::unique_ptr<FrameGrabber>    grabber;
    std::unique_ptr<H264Encoder>     encoder;
    std::unique_ptr<RtpPushStreamer> pusher;  // push mode only
};

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    SetConsoleOutputCP(CP_UTF8);

    Config cfg = ParseArgs(argc, argv);

    // Print configuration summary.
    std::cout << "=== GigERTSPStreamer ===\n"
              << "  GigECameraModule : " << cfg.grpc_addr << "\n"
              << "  Cameras          : ";
    for (size_t i = 0; i < cfg.cameras.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << cfg.cameras[i];
    }
    std::cout << "\n  Mode             : " << cfg.mode << "\n";
    if (cfg.mode == "server")
        std::cout << "  RTSP port        : " << cfg.rtsp_port << "\n";
    else
        std::cout << "  Push target      : " << cfg.push_host << ":" << cfg.push_port << "\n";
    std::cout << "  FPS              : " << cfg.fps     << "\n"
              << "  Bitrate          : " << cfg.bitrate << " kbps\n\n";

    // ── 1. Create frame grabbers and wait for the first frame per camera ──────

    std::vector<CameraSlot> slots;
    slots.reserve(cfg.cameras.size());

    for (int32_t cam_id : cfg.cameras) {
        CameraSlot slot;
        slot.camera_id = cam_id;
        slot.grabber   = std::make_unique<FrameGrabber>(cfg.grpc_addr, cam_id);
        slot.encoder   = std::make_unique<H264Encoder>();

        if (!slot.grabber->Start()) {
            std::cerr << "[Main] FATAL: FrameGrabber failed to start for camera "
                      << cam_id << ".\n";
            return 1;
        }
        slots.push_back(std::move(slot));
    }

    std::cout << "[Main] Waiting for first frame from all cameras...\n";
    for (auto& slot : slots) {
        if (!slot.grabber->WaitFirstFrame(10000)) {
            std::cerr << "[Main] FATAL: No frame from camera " << slot.camera_id
                      << " within 10 s. Is GigECameraModule running and acquiring?\n";
            for (auto& s : slots) s.grabber->Stop();
            return 1;
        }

        const int w = slot.grabber->Width();
        const int h = slot.grabber->Height();
        std::cout << "[Main] Camera " << slot.camera_id
                  << " — first frame " << w << "x" << h << "\n";

        if (!slot.encoder->Init(w, h, cfg.fps, cfg.bitrate)) {
            std::cerr << "[Main] FATAL: H264Encoder init failed for camera "
                      << slot.camera_id << ".\n"
                         "  Install FFmpeg with libx264:\n"
                         "  vcpkg install ffmpeg[x264] --triplet x64-windows\n";
            for (auto& s : slots) s.grabber->Stop();
            return 1;
        }
    }

    // ── 2. Start streaming ────────────────────────────────────────────────────

    if (cfg.mode == "server") {
        // RTSP server mode: each camera accessible at rtsp://host:<port>/camN.
        RtspServer server;
        for (auto& slot : slots)
            server.AddStream(slot.camera_id, *slot.grabber, *slot.encoder);

        std::thread server_thread([&] {
            server.Start(cfg.rtsp_port, cfg.fps);
        });

        while (!g_shutdown.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::cout << "[Main] Shutting down...\n";
        server.Stop();
        if (server_thread.joinable()) server_thread.join();

    } else {
        // Push mode: camera N → push_host:(push_port + N*2).
        for (size_t i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i];
            const uint16_t port = static_cast<uint16_t>(cfg.push_port + i * 2);
            slot.pusher = std::make_unique<RtpPushStreamer>();
            if (!slot.pusher->Start(*slot.grabber, *slot.encoder,
                                    cfg.push_host, port, cfg.fps)) {
                std::cerr << "[Main] FATAL: RtpPushStreamer failed for camera "
                          << slot.camera_id << ".\n";
                for (auto& s : slots) {
                    s.grabber->Stop();
                    if (s.pusher) s.pusher->Stop();
                }
                return 1;
            }
        }

        while (!g_shutdown.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::cout << "[Main] Shutting down...\n";
        for (auto& slot : slots) if (slot.pusher) slot.pusher->Stop();
    }

    // ── 3. Cleanup ────────────────────────────────────────────────────────────
    for (auto& slot : slots) slot.grabber->Stop();
    std::cout << "[Main] Clean exit.\n";
    return 0;
}
