/**
 * camera_control.cpp
 * ~~~~~~~~~~~~~~~~~~
 * Demonstrates every control API on a live camera:
 *   - Inspect camera info
 *   - Adjust exposure, gain, gamma, frame rate
 *   - Change region of interest (ROI)
 *   - Use grab_wait() to process each frame exactly once
 *   - Save frames to disk
 *
 * Build: same CMakeLists.txt as simple_grab (target name: camera_control)
 * Run:
 *   build\camera_control.exe [address] [camera_id]
 *   build\camera_control.exe localhost:50051 0
 */

#include "gige_camera.h"

#include <cstdio>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static void print_separator(const char* title) {
    std::printf("\n-- %s %s\n", title,
                std::string(50 - std::strlen(title), '-').c_str());
}

static void check(bool ok, const char* what) {
    std::printf("  %-40s  %s\n", what, ok ? "OK" : "FAILED");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    const std::string addr      = (argc > 1) ? argv[1] : "localhost:50051";
    const int32_t     camera_id = (argc > 2) ? std::stoi(argv[2]) : 0;

    std::printf("=== GigE Camera Control Demo ===\n");
    std::printf("Server    : %s\n", addr.c_str());
    std::printf("Camera ID : %d\n\n", camera_id);

    // ── 1. Connect ────────────────────────────────────────────────────────────
    // The constructor creates a gRPC channel. It is non-blocking; the
    // connection is established on the first RPC call below.
    GigECamera cam(addr);

    // ── 2. System state ───────────────────────────────────────────────────────
    print_separator("System state");
    {
        auto s = cam.state();
        std::printf("  Status          : %s\n",  s.status.c_str());
        std::printf("  Connected cams  : %d\n",  s.connected_cameras);
        std::printf("  Current FPS     : %.1f\n", s.current_fps);
    }

    // ── 3. Camera info ────────────────────────────────────────────────────────
    print_separator("Camera info");
    {
        auto info = cam.info(camera_id);
        if (!info) {
            std::fprintf(stderr,
                "  Camera %d not available. Is GigECameraModule.exe running?\n",
                camera_id);
            return 1;
        }
        std::printf("  Model           : %s\n",   info->model_name.c_str());
        std::printf("  Serial          : %s\n",   info->serial.c_str());
        std::printf("  IP address      : %s\n",   info->ip_address.c_str());
        std::printf("  Sensor size     : %dx%d\n", info->width, info->height);
        std::printf("  ROI offset      : (%d, %d)\n", info->offset_x, info->offset_y);
        std::printf("  Binning H/V     : %d / %d\n", info->binning_h, info->binning_v);
        std::printf("  Exposure        : %.0f us\n",  info->exposure_us);
        std::printf("  Gain            : %.2f dB\n",  info->gain_db);
        std::printf("  Gamma           : %.2f\n",     info->gamma);
        std::printf("  Frame rate      : %.1f fps\n", info->fps);
        std::printf("  Acquiring       : %s\n",   info->acquiring ? "yes" : "no");
    }

    // ── 4. Adjust camera parameters ───────────────────────────────────────────
    //
    // All setters:
    //   set_exposure(microseconds, camera_id=-1)   exposure time in µs
    //   set_gain    (dB,           camera_id=-1)   digital gain
    //   set_gamma   (value,        camera_id=-1)   gamma correction
    //   set_frame_rate(fps,        camera_id=-1)   acquisition frame rate
    //   set_roi(w, h, offset_x, offset_y, camera_id=-1)
    //   set_param(node_name, float_value, int_value, camera_id=-1)
    //
    // Pass camera_id=-1 to apply the setting to ALL cameras simultaneously.
    // ─────────────────────────────────────────────────────────────────────────

    print_separator("Adjusting parameters");

    // Exposure: 8 ms on this camera only.
    check(cam.set_exposure(8000.f, camera_id), "set_exposure(8000 us)");

    // Gain: 2 dB.
    check(cam.set_gain(2.f, camera_id), "set_gain(2 dB)");

    // Gamma: linear (1.0).
    check(cam.set_gamma(1.0f, camera_id), "set_gamma(1.0)");

    // Frame rate: 15 fps.
    check(cam.set_frame_rate(15.f, camera_id), "set_frame_rate(15 fps)");

    // ROI: 1280×720 centered crop (adjust offsets to match your sensor).
    // set_roi() resets offsets to zero first, then applies width/height, then offsets
    // so all intermediate states are within sensor bounds.
    check(cam.set_roi(1280, 720, 320, 180, camera_id),
          "set_roi(1280x720, offset 320,180)");

    // Custom GenICam node (float or integer):
    //   check(cam.set_param("BlackLevel", 4.0f, 0, camera_id), "set_param BlackLevel");
    //   check(cam.set_param("BinningHorizontal", 0.f, 2, camera_id), "binning 2x");

    // ── 5. Start acquisition ──────────────────────────────────────────────────
    print_separator("Acquisition");

    cam.start(camera_id);
    std::printf("  Acquisition started.\n");

    // ── 6. Grab frames with grab_wait() ───────────────────────────────────────
    //
    // grab_wait(camera_id, last_ts, timeout_ms) blocks until a frame arrives
    // whose timestamp differs from last_ts.  This guarantees we process each
    // physical frame exactly once — even at high frame rates.
    // ─────────────────────────────────────────────────────────────────────────

    print_separator("Grabbing 5 frames (grab_wait)");

    std::printf("  %-3s  %-6s  %-14s  %-13s  %-4s  %-4s  %-8s\n",
                "#", "cam", "WxH", "timestamp(ms)", "min", "max", "mean");

    int64_t last_ts = 0;
    int frames_done = 0;

    while (frames_done < 5) {
        // Block up to 2 seconds for a new frame.
        auto frame = cam.grab_wait(camera_id, last_ts, 2000);
        if (!frame) {
            std::printf("  [%d] timeout — no frame in 2 s\n", frames_done);
            continue;
        }

        last_ts = frame->timestamp_ms;

        // frame->pixels owns the data; it's an std::vector<uint8_t>.
        // Layout: R G B R G B ... interleaved, width * height * 3 bytes total.
        const auto& px = frame->pixels;
        uint8_t mn = 255, mx = 0;
        uint64_t sum = 0;
        for (uint8_t v : px) {
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
        }
        const double mean = px.empty() ? 0.0 : (double)sum / px.size();

        std::printf("  %-3d  %-6d  %5dx%-7d  %-13lld  %-4u  %-4u  %.2f\n",
                    frames_done + 1,
                    frame->camera_id,
                    frame->width, frame->height,
                    (long long)frame->timestamp_ms,
                    mn, mx, mean);

        // ── Your processing goes here ──────────────────────────────────────────
        //
        //   frame->pixels is owned — store it, move it, or pass it wherever.
        //   The SHM buffer has already been released; no cleanup needed.
        //
        //   OpenCV example:
        //     cv::Mat mat(frame->height, frame->width, CV_8UC3,
        //                 frame->pixels.data());     // zero-copy view
        //     cv::Mat bgr;
        //     cv::cvtColor(mat, bgr, cv::COLOR_RGB2BGR);
        //
        // ─────────────────────────────────────────────────────────────────────

        ++frames_done;
    }

    // ── 7. Save a frame to disk ────────────────────────────────────────────────
    //
    // The server writes the next captured frame as a JPEG to its save directory.
    // You can change the directory at runtime without restarting the server.
    // ─────────────────────────────────────────────────────────────────────────

    print_separator("Disk save");

    cam.set_save_dir("C:\\CameraImages");
    std::printf("  Save directory set to C:\\CameraImages\n");

    cam.save_next(camera_id);
    std::printf("  Next frame queued for JPEG save.\n");

    // ── 8. Restore ROI and stop ───────────────────────────────────────────────
    print_separator("Cleanup");

    // Restore full-sensor ROI (adjust to your actual sensor dimensions):
    cam.set_roi(4096, 2160, 0, 0, camera_id);
    std::printf("  ROI restored to full sensor.\n");

    cam.stop(camera_id);
    std::printf("  Acquisition stopped.\n\n");

    return 0;
}
