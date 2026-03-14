/**
 * simple_grab.cpp
 * ~~~~~~~~~~~~~~~
 * Minimal example: connect, start, grab 10 frames, stop.
 *
 * Build:
 *   cmake -G "Ninja"
 *         -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
 *         -DVCPKG_TARGET_TRIPLET=x64-windows
 *         -DCMAKE_BUILD_TYPE=Release
 *         -B build .
 *   cmake --build build --parallel
 *
 * Run (no Administrator required — SHM opened read-only):
 *   build\simple_grab.exe [address] [camera_id]
 *   build\simple_grab.exe localhost:50051 0
 */

#include "gige_camera.h"

#include <cstdio>
#include <string>

int main(int argc, char* argv[])
{
    const std::string address   = (argc > 1) ? argv[1] : "localhost:50051";
    const int32_t     camera_id = (argc > 2) ? std::stoi(argv[2]) : 0;

    GigECamera cam(address);

    // ── Check server ──────────────────────────────────────────────────────────
    auto state = cam.state();
    std::printf("Server: %s  status=%s  cameras=%d\n",
                address.c_str(), state.status.c_str(), state.connected_cameras);

    // ── Print camera info ─────────────────────────────────────────────────────
    if (auto info = cam.info(camera_id)) {
        std::printf("Camera %d: %s  %dx%d  exp=%.0f us  gain=%.1f dB\n",
                    camera_id,
                    info->model_name.c_str(),
                    info->width, info->height,
                    info->exposure_us, info->gain_db);
    }

    // ── (Optional) adjust settings ────────────────────────────────────────────
    // cam.set_exposure(5'000.f, camera_id);   // 5 ms
    // cam.set_gain(3.f, camera_id);
    // cam.set_roi(1280, 720, 320, 180, camera_id);

    // ── Start acquisition ─────────────────────────────────────────────────────
    cam.start(camera_id);

    // ── Grab frames ───────────────────────────────────────────────────────────
    std::printf("\nGrabbing 10 frames...\n\n");
    std::printf("  %-5s  %-4s  %-14s  %-4s  %-4s  %-8s\n",
                "#", "cam", "WxH", "min", "max", "mean");

    for (int i = 0; i < 10; ++i) {
        auto frame = cam.grab(camera_id);
        if (!frame) {
            std::printf("  [%d] no frame available\n", i);
            continue;
        }

        // frame->pixels: R-G-B interleaved, frame->width * frame->height * 3 bytes
        const auto& px = frame->pixels;
        uint8_t mn = 255, mx = 0;
        uint64_t sum = 0;
        for (uint8_t v : px) {
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
        }
        const double mean = px.empty() ? 0.0 : static_cast<double>(sum) / px.size();

        std::printf("  %-5d  %-4d  %5dx%-7d  %-4u  %-4u  %-8.2f\n",
                    i + 1, frame->camera_id,
                    frame->width, frame->height,
                    mn, mx, mean);

        // ── Your processing goes here ─────────────────────────────────────────
        //
        //   frame->pixels  is an owned std::vector<uint8_t> — valid forever.
        //   Pixel layout: R G B interleaved, frame->width * frame->height * 3 bytes.
        //
        //   Example: wrap in an OpenCV Mat (BGR order)
        //     cv::Mat rgb(frame->height, frame->width, CV_8UC3, frame->pixels.data());
        //     cv::Mat bgr;
        //     cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
        //
        // ─────────────────────────────────────────────────────────────────────
    }

    // ── Stop acquisition ──────────────────────────────────────────────────────
    cam.stop(camera_id);

    return 0;
}
