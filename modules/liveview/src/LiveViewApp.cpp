#include "LiveViewApp.h"

#include <GL/glew.h>
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Module-level helpers
// ─────────────────────────────────────────────────────────────────────────────

static constexpr const char* kAutoModes[]   = { "Off", "Once", "Continuous" };
static constexpr int         kAutoModeCount = 3;

static int AutoModeToIdx(const std::string& s) {
    if (s == "Once")       return 1;
    if (s == "Continuous") return 2;
    return 0;
}

static const char* IdxToAutoMode(int idx) {
    return (idx >= 0 && idx < kAutoModeCount) ? kAutoModes[idx] : "Off";
}

static std::string FormatTimestampUs(int64_t us) {
    if (us <= 0) return "--:--:--.---";
    const time_t sec = static_cast<time_t>(us / 1'000'000LL);
    struct tm t{};
    localtime_s(&t, &sec);
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             t.tm_hour, t.tm_min, t.tm_sec,
             static_cast<int>((us % 1'000'000LL) / 1000LL));
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

LiveViewApp::LiveViewApp() = default;

LiveViewApp::~LiveViewApp() {
    Disconnect();
}

void LiveViewApp::Disconnect() {
    for (auto& p : panels_) {
        if (p.feed) p.feed->Stop();
        if (p.dma_fence) {
            glDeleteSync(static_cast<GLsync>(p.dma_fence));
            p.dma_fence = nullptr;
        }
        for (uint32_t* tp : {&p.texture, &p.tex_back}) {
            if (*tp) {
                GLuint t = static_cast<GLuint>(*tp);
                glDeleteTextures(1, &t);
                *tp = 0;
            }
        }
        for (int i = 0; i < 2; ++i) {
            if (p.pbo[i]) {
                const GLuint pbo = static_cast<GLuint>(p.pbo[i]);
                glDeleteBuffers(1, &pbo);
                p.pbo[i] = 0;
            }
        }
    }
    panels_.clear();
    cam_.reset();
    sys_state_          = {};
    connect_error_.clear();
    info_refresh_timer_ = 0.f;
    selected_cam_idx_   = 0;
    state_              = AppState::Connect;
}

void LiveViewApp::Connect(const std::string& addr) {
    connect_error_.clear();
    try {
        auto grpc_cam = std::make_unique<GigECamera>(addr);
        GigeSystemState sys = grpc_cam->state();

        if (sys.connected_cameras <= 0) {
            connect_error_ = "No cameras (status: " + sys.status +
                             "). Is GigECameraModule acquiring?";
            return;
        }

        cam_       = std::move(grpc_cam);
        sys_state_ = sys;

        for (int32_t i = 0; i < sys.connected_cameras; ++i) {
            CameraPanel panel;
            panel.camera_id = i;
            panel.feed      = std::make_unique<CameraFeed>(addr, i);

            auto make_tex = [&]() -> GLuint {
                GLuint t = 0; glGenTextures(1, &t);
                glBindTexture(GL_TEXTURE_2D, t);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                return t;
            };
            panel.texture  = make_tex();
            panel.tex_back = make_tex();

            if (auto inf = panel.feed->QueryInfo()) {
                panel.info = *inf;
                LoadParamsFromInfo(panel);
            }

            panel.feed->Start();
            panels_.push_back(std::move(panel));
        }

        selected_cam_idx_ = 0;
        state_            = AppState::Running;

    } catch (const std::exception& ex) {
        connect_error_ = ex.what();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GL texture upload
// ─────────────────────────────────────────────────────────────────────────────

void LiveViewApp::UploadTexture(CameraPanel& panel,
                                const uint8_t* data,
                                int w, int h, int channels) {
    using clk = std::chrono::steady_clock;
    const auto t_upload_start = clk::now();

    // Always write into tex_back.  tex_front (panel.texture) is untouched here
    // so the GPU can render from it concurrently on the graphics engine while
    // the copy engine DMAs new data into tex_back.
    const GLuint  tex     = static_cast<GLuint>(panel.tex_back);
    const GLenum  int_fmt = (channels == 1) ? GL_R8  : GL_RGB8;
    const GLenum  pix_fmt = (channels == 1) ? GL_RED : GL_BGR;
    const GLsizei sz      = static_cast<GLsizei>(w) * h * channels;

    // Lazy create both PBOs.
    for (int i = 0; i < 2; ++i) {
        if (!panel.pbo[i]) {
            GLuint p = 0; glGenBuffers(1, &p);
            panel.pbo[i] = static_cast<uint32_t>(p);
        }
    }

    // Pre-allocate (or resize on ROI change) — done once, not every frame.
    // glBufferData here is a one-time cost; the hot path uses glMapBufferRange.
    if (panel.pbo_sz != sz) {
        for (int i = 0; i < 2; ++i) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(panel.pbo[i]));
            glBufferData(GL_PIXEL_UNPACK_BUFFER, sz, nullptr, GL_STREAM_DRAW);
        }
        panel.pbo_sz    = sz;
        panel.pbo_ready = false;  // next frame: no valid previous-buffer yet
        panel.pbo_idx   = 0;
    }

    // True double-buffer:
    //   CPU writes into pbo[write_idx] (no GPU wait — UNSYNCHRONIZED).
    //   GPU DMAs from pbo[upload_idx] = last frame's buffer.
    // On the very first call (pbo_ready=false) both indices are the same so
    // the first frame is uploaded immediately without a 1-frame-stale display.
    const int write_idx  = panel.pbo_idx;
    const int upload_idx = panel.pbo_ready ? (1 - panel.pbo_idx) : panel.pbo_idx;
    panel.pbo_idx   = 1 - panel.pbo_idx;
    panel.pbo_ready = true;

    const GLuint write_pbo  = static_cast<GLuint>(panel.pbo[write_idx]);
    const GLuint upload_pbo = static_cast<GLuint>(panel.pbo[upload_idx]);

    // ── Step 1: CPU → PBO (write-combined, no GPU sync) ──────────────────────
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, write_pbo);
    void* dst = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, sz,
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT);

    if (dst) {
        memcpy(dst, data, static_cast<size_t>(sz));
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    }

    // ── Step 2: PBO[upload_idx] → VRAM texture (last frame's data) ───────────
    // Bind the upload PBO so glTexSubImage2D reads from it, not the write PBO.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, dst ? upload_pbo : 0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (w != panel.tex_back_w || h != panel.tex_back_h || channels != panel.tex_back_ch) {
        glTexImage2D(GL_TEXTURE_2D, 0, int_fmt, w, h, 0,
                     pix_fmt, GL_UNSIGNED_BYTE, dst ? nullptr : data);
        panel.tex_back_w = w; panel.tex_back_h = h; panel.tex_back_ch = channels;
        const GLint sg[4] = { GL_RED, GL_RED, GL_RED, GL_ONE };
        const GLint sr[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ONE };
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA,
                         (channels == 1) ? sg : sr);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        pix_fmt, GL_UNSIGNED_BYTE, dst ? nullptr : data);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    // Insert a fence immediately after the DMA command.  RenderMainView checks it
    // non-blocking before the next upload — if not signaled yet the upload is
    // skipped so we never queue DMA commands faster than the GPU can drain them.
    // (glFlush is intentionally absent: glfwSwapBuffers provides the flush, and
    // an early flush here is what caused WDDM to accumulate DMA batches → OOM.)
    if (panel.dma_fence)
        glDeleteSync(static_cast<GLsync>(panel.dma_fence));
    panel.dma_fence      = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    panel.tex_back_ready = true;  // signal RenderMainView to swap at next frame start
    panel.transfer_ms = std::chrono::duration<float, std::milli>(
        clk::now() - t_upload_start).count();
}

// ─────────────────────────────────────────────────────────────────────────────
// Top-level render dispatch
// ─────────────────────────────────────────────────────────────────────────────

void LiveViewApp::Render(float delta_time_s) {
    if (state_ == AppState::Connect) {
        RenderConnectDialog();
        return;
    }
    RenderMainView(delta_time_s);
}

// ─────────────────────────────────────────────────────────────────────────────
// Connect dialog
// ─────────────────────────────────────────────────────────────────────────────

void LiveViewApp::RenderConnectDialog() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);
    ImGui::Begin("##connect", nullptr,
        ImGuiWindowFlags_NoResize         | ImGuiWindowFlags_NoMove    |
        ImGuiWindowFlags_NoCollapse       | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

    {
        const char* title = "GigE Camera Live View";
        const float tw = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX((460.f - tw) * 0.5f);
        ImGui::TextUnformatted(title);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("GigECameraModule gRPC address:");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.f);
    const bool enter = ImGui::InputText("##addr", addr_buf_, sizeof(addr_buf_),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();
    if (ImGui::Button("Connect", ImVec2(-1.f, 0.f)) || enter)
        Connect(addr_buf_);

    if (!connect_error_.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.35f, 0.35f, 1.f));
        ImGui::TextWrapped("Error: %s", connect_error_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
// Main view — toolbar + image area + sidebar
// ─────────────────────────────────────────────────────────────────────────────

void LiveViewApp::RenderMainView(float delta_time_s) {
    // ── Periodic data refresh ─────────────────────────────────────────────────
    info_refresh_timer_ += delta_time_s;
    if (info_refresh_timer_ >= kInfoRefreshSecs) {
        info_refresh_timer_ = 0.f;
        if (cam_) sys_state_ = cam_->state();
        for (auto& p : panels_) {
            if (auto inf = p.feed->QueryInfo())
                p.info = *inf;
        }
    }

    // ── Swap tex_back → tex_front for panels whose DMA finished last frame ─────
    // The GPU submitted DMA[tex_back] and render[tex_front] in the previous frame.
    // Because they targeted different textures, NVIDIA runs them on separate engines
    // (copy engine + graphics engine) in true parallel.  Now that glfwSwapBuffers
    // has returned (GPU is done presenting), we can safely swap the references.
    for (auto& p : panels_) {
        if (p.tex_back_ready) {
            std::swap(p.texture,  p.tex_back);
            std::swap(p.tex_w,    p.tex_back_w);
            std::swap(p.tex_h,    p.tex_back_h);
            std::swap(p.tex_ch,   p.tex_back_ch);
            p.tex_back_ready = false;
        }
    }

    // ── Drain new frames into GL textures (all cameras, not just selected) ────
    for (auto& p : panels_) {
        // Non-blocking fence check: if the GPU is still DMAs-ing last frame's tex_back,
        // skip this upload entirely.  Do NOT consume IsNewFrame — it stays true so the
        // next render call picks up the latest camera frame.  This prevents submitting
        // DMA commands faster than the GPU can drain them (which caused OOM on WDDM).
        if (p.dma_fence) {
            const GLenum s = glClientWaitSync(
                static_cast<GLsync>(p.dma_fence), 0, 0);
            if (s == GL_TIMEOUT_EXPIRED || s == GL_WAIT_FAILED)
                continue;  // GPU still busy — skip, render with current tex_front
            glDeleteSync(static_cast<GLsync>(p.dma_fence));
            p.dma_fence = nullptr;
        }

        if (!p.feed->IsNewFrame()) continue;

        // Zero-copy path: pin SHM slot → memcpy SHM→PBO (one copy) → release pin.
        // PBO memory is pre-committed by the GL driver so no VirtualAlloc/zero-page faults.
        auto pin = p.feed->TryPinLatest();
        if (pin) {
            UploadTexture(p, pin->data, pin->width, pin->height, pin->channels);
            p.last_ts_us = pin->timestamp_us;
            p.feed->ReleasePin(pin->_slot_idx);
        } else {
            // Fallback: RW SHM unavailable (not admin) — use heap buffer via gRPC.
            // fallback_frame_ keeps its pixel buffer capacity warm across calls.
            if (p.feed->GrabInto(p.fallback_frame_)) {
                UploadTexture(p, p.fallback_frame_.pixels.data(),
                              p.fallback_frame_.width, p.fallback_frame_.height,
                              p.fallback_frame_.channels);
                p.last_ts_us = p.fallback_frame_.timestamp_us;
            }
        }
    }

    // ── Full-screen root window ───────────────────────────────────────────────
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar            | ImGuiWindowFlags_NoResize    |
        ImGuiWindowFlags_NoScrollbar           | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);

    // ── Toolbar ───────────────────────────────────────────────────────────────
    if (ImGui::Button("Disconnect")) {
        Disconnect();
        ImGui::End();
        return;
    }
    ImGui::SameLine();

    // Start / Stop acquisition
    const bool acquiring = (sys_state_.status == "ACQUIRING" ||
                            sys_state_.status == "PARTIAL");
    if (acquiring) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.12f, 0.12f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.20f, 0.20f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.85f, 0.30f, 0.30f, 1.f));
        if (ImGui::Button("Stop Capture")) {
            if (cam_) cam_->stop(-1);
            sys_state_ = cam_->state();
        }
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.45f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.60f, 0.22f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.28f, 0.72f, 0.30f, 1.f));
        if (ImGui::Button("Start Capture")) {
            if (cam_) cam_->start(-1);
            sys_state_ = cam_->state();
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::SameLine();

    ImGui::Text("| %s | Cams: %d | Agg: %.1f fps",
                sys_state_.status.c_str(),
                sys_state_.connected_cameras,
                sys_state_.current_fps);

    // Camera selector — only shown when > 1 camera connected
    if (static_cast<int>(panels_.size()) > 1) {
        ImGui::SameLine(0.f, 16.f);
        ImGui::Text("|");
        ImGui::SameLine(0.f, 16.f);
        for (int i = 0; i < static_cast<int>(panels_.size()); ++i) {
            if (i > 0) ImGui::SameLine(0.f, 4.f);
            char buf[20];
            snprintf(buf, sizeof(buf), " Camera %d ", i);
            const bool active = (i == selected_cam_idx_);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::Button(buf))
                selected_cam_idx_ = i;
            if (active)
                ImGui::PopStyleColor();
        }
    }

    // Sidebar toggle button — right-aligned
    {
        const char* lbl = sidebar_open_ ? "Info/Settings <<" : ">> Info/Settings";
        const float btn_w = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - btn_w);
        if (ImGui::Button(lbl))
            sidebar_open_ = !sidebar_open_;
    }

    ImGui::Separator();

    // ── Guard ─────────────────────────────────────────────────────────────────
    if (selected_cam_idx_ >= static_cast<int>(panels_.size()))
        selected_cam_idx_ = 0;
    auto& panel = panels_[selected_cam_idx_];

    // ── Content area (image | sidebar) ────────────────────────────────────────
    const ImGuiStyle& style   = ImGui::GetStyle();
    const float       avail_w = ImGui::GetContentRegionAvail().x;
    const float       avail_h = ImGui::GetContentRegionAvail().y;
    const float       side_w  = sidebar_open_ ? kSidebarWidth : 0.f;
    const float       img_w   = avail_w - side_w
                                - (sidebar_open_ ? style.ItemSpacing.x : 0.f);

    // Image child — no border, no scrollbar
    ImGui::BeginChild("##imgarea", ImVec2(img_w, avail_h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    RenderImagePanel(panel, img_w, avail_h);
    ImGui::EndChild();

    // Sidebar child — border + vertical scrollbar for overflow
    if (sidebar_open_) {
        ImGui::SameLine();
        ImGui::BeginChild("##sidebar", ImVec2(side_w, avail_h), true);
        RenderSidebar(panel);
        ImGui::EndChild();
    }

    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
// Image panel
// ─────────────────────────────────────────────────────────────────────────────

void LiveViewApp::RenderImagePanel(CameraPanel& panel, float avail_w, float avail_h) {
    if (panel.tex_w > 0 && panel.tex_h > 0) {
        const float aspect = static_cast<float>(panel.tex_w) / static_cast<float>(panel.tex_h);
        float img_w = avail_w;
        float img_h = avail_h;
        if (img_w / img_h > aspect)
            img_w = img_h * aspect;
        else
            img_h = img_w / aspect;

        ImGui::SetCursorPos(ImVec2((avail_w - img_w) * 0.5f,
                                   (avail_h - img_h) * 0.5f));
        ImGui::Image((ImTextureID)(uintptr_t)panel.texture, ImVec2(img_w, img_h));
    } else {
        ImGui::Dummy(ImVec2(avail_w, avail_h));
        const ImVec2 p0 = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddRectFilled(
            p0, ImVec2(p0.x + avail_w, p0.y + avail_h), IM_COL32(30, 30, 30, 255));
        const char* msg  = "Waiting for frame...";
        const ImVec2 tsz = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(p0.x + (avail_w - tsz.x) * 0.5f,
                   p0.y + (avail_h - tsz.y) * 0.5f),
            IM_COL32(160, 160, 160, 255), msg);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Sidebar — tab bar dispatching Info / Parameters
// ─────────────────────────────────────────────────────────────────────────────

void LiveViewApp::RenderSidebar(CameraPanel& panel) {
    if (ImGui::BeginTabBar("##sidebartabs")) {
        if (ImGui::BeginTabItem("Info"))       { RenderInfoTab(panel);  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Parameters")) { RenderParamTab(panel); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Info tab
// ─────────────────────────────────────────────────────────────────────────────

void LiveViewApp::RenderInfoTab(CameraPanel& panel) {
    const auto& inf = panel.info;

    // ── Identity ──────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Identity");
    ImGui::Text("Camera ID : %d",  inf.camera_id);
    ImGui::Text("Model     : %s",  inf.model_name.empty() ? "?" : inf.model_name.c_str());
    ImGui::Text("Serial    : %s",  inf.serial.empty()     ? "?" : inf.serial.c_str());
    ImGui::Text("IP        : %s",  inf.ip_address.empty() ? "?" : inf.ip_address.c_str());
    ImGui::Text("Status    : %s",  inf.acquiring ? "Acquiring" : "Stopped");

    // ── Image geometry ────────────────────────────────────────────────────────
    ImGui::SeparatorText("Geometry");
    ImGui::Text("Size      : %d x %d px",   inf.width, inf.height);
    ImGui::Text("Offset    : %d, %d",        inf.offset_x, inf.offset_y);
    ImGui::Text("Binning   : %dH x %dV",    inf.binning_h, inf.binning_v);

    // ── Exposure & gain ───────────────────────────────────────────────────────
    ImGui::SeparatorText("Exposure & Gain");
    ImGui::Text("Exposure  : %.0f us (%.2f ms)", inf.exposure_us, inf.exposure_us * 0.001f);
    ImGui::Text("Exp Auto  : %s", inf.exposure_auto.empty() ? "?" : inf.exposure_auto.c_str());
    ImGui::Text("Gain      : %.2f dB", inf.gain_db);
    ImGui::Text("Gain Auto : %s", inf.gain_auto.empty() ? "?" : inf.gain_auto.c_str());
    ImGui::Text("Gamma     : %.3f",    inf.gamma);
    ImGui::Text("BlackLvl  : %.3f",    inf.black_level);
    ImGui::Text("EV Comp   : %.2f EV", inf.ev_compensation);

    // ── Frame rate ────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Frame Rate");
    ImGui::Text("Configured: %.1f fps", inf.frame_rate);
    ImGui::Text("Camera FPS: %.1f fps", inf.fps);  // SDK-measured on server
    if (inf.exposure_us > 0.f) {
        const float exp_limit = 1e6f / inf.exposure_us;
        if (exp_limit < inf.frame_rate + 1.f) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.85f, 0.2f, 1.f));
            ImGui::Text("Exp limit : %.1f fps", exp_limit);
            ImGui::PopStyleColor();
            ImGui::TextDisabled("  Exposure %.0f us caps max fps", inf.exposure_us);
        }
    }

    // ── Link ─────────────────────────────────────────────────────────────────
    if (inf.link_speed_bps > 0) {
        ImGui::SeparatorText("Link");
        ImGui::Text("Link Speed: %d MB/s",
                    static_cast<int>(inf.link_speed_bps / 1'000'000));
    }

    // ── Last frame ────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Last Frame");
    ImGui::Text("Timestamp : %s", FormatTimestampUs(panel.last_ts_us).c_str());
    ImGui::Text("Buffer sz : %d x %d px", panel.tex_w, panel.tex_h);

    // ── Client-side performance ───────────────────────────────────────────────
    ImGui::SeparatorText("Client Performance");
    ImGui::Text("Live FPS  : %.1f fps", panel.feed->GetFps());
    if (panel.feed->UsingDirectGrab())
        ImGui::TextDisabled("  Direct SHM");
    else
        ImGui::TextDisabled("  gRPC fallback (run GigECameraModule as Admin for Direct SHM)");
    ImGui::Text("Transfer  : %.2f ms", panel.transfer_ms);
    ImGui::TextDisabled("  SHM -> PBO (one memcpy, Debayer (GPU))");

    ImGui::Spacing();
    if (ImGui::Button("Refresh Now", ImVec2(-1.f, 0.f))) {
        if (auto inf2 = panel.feed->QueryInfo()) {
            panel.info       = *inf2;
            panel.params.loaded = false;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameters tab — helpers
// ─────────────────────────────────────────────────────────────────────────────

void LiveViewApp::LoadParamsFromInfo(CameraPanel& panel) {
    const auto& inf = panel.info;
    auto& ps        = panel.params;

    ps.exposure_us       = inf.exposure_us > 0.f ? inf.exposure_us : 1000.f;
    ps.gain_db           = inf.gain_db;
    ps.gamma             = inf.gamma  > 0.f ? inf.gamma  : 1.f;
    ps.frame_rate        = inf.frame_rate > 0.f ? inf.frame_rate : 30.f;
    ps.ev_compensation   = inf.ev_compensation;
    ps.exposure_auto_idx = AutoModeToIdx(inf.exposure_auto);
    ps.gain_auto_idx     = AutoModeToIdx(inf.gain_auto);

    ps.roi_width    = std::max(8, inf.width);
    ps.roi_height   = std::max(8, inf.height);
    ps.roi_offset_x = std::max(0, inf.offset_x);
    ps.roi_offset_y = std::max(0, inf.offset_y);
    ps.binning_h    = std::max(1, inf.binning_h);
    ps.binning_v    = std::max(1, inf.binning_v);

    // link_speed_bps in GigeCameraInfo (bytes/s) → MB/s for the UI
    if (inf.link_speed_bps > 0)
        ps.link_speed_mbps = static_cast<int>(inf.link_speed_bps / 1'000'000LL);
    else if (!ps.loaded)
        ps.link_speed_mbps = 600;  // first load default

    ps.loaded = true;
}

void LiveViewApp::ApplyLiveParams(CameraPanel& panel) {
    if (!cam_) return;
    auto& ps          = panel.params;
    const int32_t cid = panel.camera_id;
    std::string failed;

    auto try_set = [&](bool ok, const char* name) {
        if (!ok) { if (!failed.empty()) failed += ", "; failed += name; }
    };

    // Apply auto modes first so manual values aren't immediately overridden.
    try_set(cam_->set_exposure_auto(IdxToAutoMode(ps.exposure_auto_idx), cid), "ExposureAuto");
    if (ps.exposure_auto_idx == 0)
        try_set(cam_->set_exposure(ps.exposure_us, cid), "Exposure");

    try_set(cam_->set_gain_auto(IdxToAutoMode(ps.gain_auto_idx), cid), "GainAuto");
    if (ps.gain_auto_idx == 0)
        try_set(cam_->set_gain(ps.gain_db, cid), "Gain");

    try_set(cam_->set_gamma(ps.gamma, cid), "Gamma");

    // AcquisitionFrameRateEnable: boolean node, non-fatal if camera doesn't expose it.
    cam_->set_param("AcquisitionFrameRateEnable", 0.f,
                    ps.frame_rate_limit_enabled ? 1 : 0, cid);
    if (ps.frame_rate_limit_enabled && ps.frame_rate > 0.f)
        try_set(cam_->set_frame_rate(ps.frame_rate, cid), "FrameRate");

    // EV compensation is only writable when auto-exposure is active.
    if (ps.exposure_auto_idx != 0)
        cam_->set_ev_compensation(ps.ev_compensation, cid);

    ps.apply_status = failed.empty()
        ? "[OK] Live params applied."
        : "[!] Failed: " + failed;

    if (auto inf = panel.feed->QueryInfo()) {
        panel.info  = *inf;
        ps.loaded   = false;
        LoadParamsFromInfo(panel);
    }
}

void LiveViewApp::ApplyStopRequiredParams(CameraPanel& panel) {
    if (!cam_) return;
    auto& ps          = panel.params;
    const int32_t cid = panel.camera_id;
    std::string failed;

    auto try_set = [&](bool ok, const char* name) {
        if (!ok) { if (!failed.empty()) failed += ", "; failed += name; }
    };

    ps.apply_status = "Stopping camera...";
    cam_->stop(cid);

    // ROI (offsets cleared first to avoid geometry constraint violations)
    try_set(cam_->set_roi(ps.roi_width, ps.roi_height,
                          ps.roi_offset_x, ps.roi_offset_y, cid), "ROI");

    // Binning
    try_set(cam_->set_param("BinningHorizontal", 0.f, ps.binning_h, cid), "BinningH");
    try_set(cam_->set_param("BinningVertical",   0.f, ps.binning_v, cid), "BinningV");

    // Link speed: not all cameras expose this as writable — non-fatal.
    const int32_t link_bps =
        static_cast<int32_t>(static_cast<int64_t>(ps.link_speed_mbps) * 1'000'000LL);
    const bool link_ok = cam_->set_param("DeviceLinkThroughputLimit", 0.f, link_bps, cid);

    cam_->start(cid);

    if (!failed.empty())
        ps.apply_status = "[!] Failed: " + failed
                          + (link_ok ? "" : ", LinkSpeed");
    else if (!link_ok)
        ps.apply_status = "[OK] Applied. Link speed unchanged (read-only on this camera).";
    else
        ps.apply_status = "[OK] Applied — camera restarted.";

    if (auto inf = panel.feed->QueryInfo()) {
        panel.info = *inf;
        ps.loaded  = false;
        LoadParamsFromInfo(panel);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameters tab — render
// ─────────────────────────────────────────────────────────────────────────────

void LiveViewApp::RenderParamTab(CameraPanel& panel) {
    auto& ps = panel.params;
    if (!ps.loaded)
        LoadParamsFromInfo(panel);

    const float lbl_w = 110.f;   // label column width

    // ── Live parameters ───────────────────────────────────────────────────────
    ImGui::SeparatorText("Live Parameters");
    ImGui::TextDisabled("Changes take effect immediately.");
    ImGui::Spacing();

    // Exposure Auto — checkbox (Continuous) + Once button
    {
        bool exp_auto = (ps.exposure_auto_idx == 2);  // 2=Continuous
        ImGui::Text("Auto Exp:");
        ImGui::SameLine(lbl_w);
        if (ImGui::Checkbox("##expauto", &exp_auto)) {
            ps.exposure_auto_idx = exp_auto ? 2 : 0;
            const char* mode = exp_auto ? "Continuous" : "Off";
            if (cam_) cam_->set_exposure_auto(mode, panel.camera_id);
            ps.apply_status = exp_auto ? "[OK] Auto exposure on." : "[OK] Auto exposure off.";
        }
        ImGui::SameLine();
        ImGui::TextDisabled(exp_auto ? "Continuous" : "Off");
        ImGui::SameLine();
        if (ImGui::SmallButton("Once##exponce")) {
            ps.exposure_auto_idx = 1;
            if (cam_) cam_->set_exposure_auto("Once", panel.camera_id);
            ps.apply_status = "[OK] Exposure once triggered.";
        }
    }

    // Exposure value (greyed when auto is active; Set button applies immediately)
    {
        const bool manual = (ps.exposure_auto_idx == 0);
        if (!manual) ImGui::BeginDisabled();
        ImGui::Text("Exposure:");
        ImGui::SameLine(lbl_w);
        ImGui::SetNextItemWidth(-60.f);
        ImGui::InputFloat("##exp", &ps.exposure_us, 100.f, 1000.f, "%.0f us");
        ps.exposure_us = std::max(1.f, ps.exposure_us);
        ImGui::SameLine();
        if (ImGui::SmallButton("Set##expset")) {
            if (cam_) {
                const bool ok = cam_->set_exposure(ps.exposure_us, panel.camera_id);
                ps.apply_status = ok ? "[OK] Exposure set." : "[!] Failed: Exposure";
            }
        }
        if (!manual) ImGui::EndDisabled();
    }

    // Gain Auto — checkbox (Continuous) + Once button
    {
        bool gain_auto = (ps.gain_auto_idx == 2);
        ImGui::Text("Auto Gain:");
        ImGui::SameLine(lbl_w);
        if (ImGui::Checkbox("##gainauto", &gain_auto)) {
            ps.gain_auto_idx = gain_auto ? 2 : 0;
            const char* mode = gain_auto ? "Continuous" : "Off";
            if (cam_) cam_->set_gain_auto(mode, panel.camera_id);
            ps.apply_status = gain_auto ? "[OK] Auto gain on." : "[OK] Auto gain off.";
        }
        ImGui::SameLine();
        ImGui::TextDisabled(gain_auto ? "Continuous" : "Off");
        ImGui::SameLine();
        if (ImGui::SmallButton("Once##gainonce")) {
            ps.gain_auto_idx = 1;
            if (cam_) cam_->set_gain_auto("Once", panel.camera_id);
            ps.apply_status = "[OK] Gain once triggered.";
        }
    }

    // Gain value (greyed when auto is active)
    {
        const bool manual = (ps.gain_auto_idx == 0);
        if (!manual) ImGui::BeginDisabled();
        ImGui::Text("Gain:");
        ImGui::SameLine(lbl_w);
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputFloat("##gain", &ps.gain_db, 0.5f, 5.f, "%.2f dB");
        ps.gain_db = std::max(0.f, ps.gain_db);
        if (!manual) ImGui::EndDisabled();
    }

    // Gamma
    ImGui::Text("Gamma:");
    ImGui::SameLine(lbl_w);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::SliderFloat("##gamma", &ps.gamma, 0.5f, 4.0f, "%.3f");

    // Frame Rate
    ImGui::Text("FPS Limit:");
    ImGui::SameLine(lbl_w);
    ImGui::Checkbox("##fpsenable", &ps.frame_rate_limit_enabled);
    ImGui::SameLine();
    ImGui::TextDisabled(ps.frame_rate_limit_enabled ? "Enabled" : "Disabled");

    if (!ps.frame_rate_limit_enabled) ImGui::BeginDisabled();
    ImGui::Text("Frame Rate:");
    ImGui::SameLine(lbl_w);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputFloat("##fps", &ps.frame_rate, 1.f, 5.f, "%.1f fps");
    ps.frame_rate = std::max(1.f, ps.frame_rate);
    if (!ps.frame_rate_limit_enabled) ImGui::EndDisabled();

    // EV Compensation — greyed when ExposureAuto is Off
    {
        const bool ev_active = (ps.exposure_auto_idx != 0);
        if (!ev_active) ImGui::BeginDisabled();
        ImGui::Text("EV Comp:");
        ImGui::SameLine(lbl_w);
        ImGui::SetNextItemWidth(-1.f);
        ImGui::SliderFloat("##ev", &ps.ev_compensation, -3.f, 3.f, "%.1f EV");
        if (!ev_active) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(needs Auto)");
        }
    }

    // Debayer toggle — immediate apply; switches SHM between BGR8 (3ch) and raw Bayer (1ch)
    {
        ImGui::Text("Debayer:");
        ImGui::SameLine(lbl_w);
        bool deb = ps.debayer_enabled;
        if (ImGui::Checkbox("##debayer", &deb)) {
            ps.debayer_enabled = deb;
            if (cam_) {
                const bool ok = cam_->set_debayer_mode(deb ? "On" : "Off", panel.camera_id);
                ps.apply_status = ok
                    ? (deb ? "[OK] Debayer on (BGR8)." : "[OK] Debayer off (raw Bayer).")
                    : "[!] Failed: DebayerMode";
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled(deb ? "On (BGR8)" : "Off (raw Bayer, 1ch)");
    }

    ImGui::Spacing();
    if (ImGui::Button("Apply Live Params", ImVec2(-1.f, 0.f)))
        ApplyLiveParams(panel);

    ImGui::Spacing();

    // ── Stop-required parameters ──────────────────────────────────────────────
    ImGui::SeparatorText("Stop-Required Parameters");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.85f, 0.2f, 1.f));
    ImGui::TextWrapped("These will stop and restart acquisition.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // ROI
    ImGui::Text("ROI Width:");
    ImGui::SameLine(lbl_w);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputInt("##roiw", &ps.roi_width, 8, 64);
    ps.roi_width = std::max(8, ps.roi_width);

    ImGui::Text("ROI Height:");
    ImGui::SameLine(lbl_w);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputInt("##roih", &ps.roi_height, 8, 64);
    ps.roi_height = std::max(8, ps.roi_height);

    ImGui::Text("Offset X:");
    ImGui::SameLine(lbl_w);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputInt("##offx", &ps.roi_offset_x, 4, 32);
    ps.roi_offset_x = std::max(0, ps.roi_offset_x);

    ImGui::Text("Offset Y:");
    ImGui::SameLine(lbl_w);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputInt("##offy", &ps.roi_offset_y, 4, 32);
    ps.roi_offset_y = std::max(0, ps.roi_offset_y);

    // Binning
    ImGui::Text("Binning H:");
    ImGui::SameLine(lbl_w);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputInt("##binh", &ps.binning_h, 1, 1);
    ps.binning_h = std::max(1, ps.binning_h);

    ImGui::Text("Binning V:");
    ImGui::SameLine(lbl_w);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputInt("##binv", &ps.binning_v, 1, 1);
    ps.binning_v = std::max(1, ps.binning_v);

    // Link Speed
    ImGui::Text("Link Speed:");
    ImGui::SameLine(lbl_w);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputInt("##link", &ps.link_speed_mbps, 10, 100);
    ps.link_speed_mbps = std::clamp(ps.link_speed_mbps, 1, 1200);
    ImGui::TextDisabled("  MB/s  (×1 000 000 = bytes/s)");

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.60f, 0.22f, 0.04f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.76f, 0.32f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.88f, 0.44f, 0.16f, 1.f));
    const bool do_stop = ImGui::Button("Apply  (will stop camera)", ImVec2(-1.f, 0.f));
    ImGui::PopStyleColor(3);

    if (do_stop)
        ApplyStopRequiredParams(panel);

    // Apply status feedback
    if (!ps.apply_status.empty()) {
        ImGui::Spacing();
        const bool is_ok = (ps.apply_status.rfind("[OK]", 0) == 0);
        ImGui::PushStyleColor(ImGuiCol_Text,
            is_ok ? ImVec4(0.4f, 1.0f, 0.4f, 1.f) : ImVec4(1.f, 0.65f, 0.2f, 1.f));
        ImGui::TextWrapped("%s", ps.apply_status.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // Reload params from server
    if (ImGui::Button("Reload from Camera", ImVec2(-1.f, 0.f))) {
        if (auto inf = panel.feed->QueryInfo()) {
            panel.info  = *inf;
            ps.loaded   = false;
            LoadParamsFromInfo(panel);
            ps.apply_status = "[OK] Params reloaded from camera.";
        }
    }
}
