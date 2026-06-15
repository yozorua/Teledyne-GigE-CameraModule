#pragma once

#include "CameraFeed.h"
#include "gige_camera.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Editable copy of camera parameters shown in the Settings panel.
// ─────────────────────────────────────────────────────────────────────────────

struct ParamEditState {
    // ── Live-adjustable (no acquisition stop needed) ──────────────────────────
    float exposure_us{1000.f};
    float gain_db{0.f};
    float gamma{1.f};
    float frame_rate{30.f};
    bool  frame_rate_limit_enabled{false};  // AcquisitionFrameRateEnable
    float ev_compensation{0.f};
    int   exposure_auto_idx{0};  // 0=Off  1=Once  2=Continuous
    int   gain_auto_idx{0};

    // ── Requires full stop before applying ────────────────────────────────────
    int   roi_width{1920};
    int   roi_height{1080};
    int   roi_offset_x{0};
    int   roi_offset_y{0};
    int   binning_h{1};
    int   binning_v{1};
    int   link_speed_mbps{600};   // UI in MB/s; sent as bytes/s (× 1 000 000)

    bool        debayer_enabled{true};  // true → GPU shader debayers in LiveView; false → grayscale
    std::string apply_status;     // feedback shown after Apply
    bool        loaded{false};    // false → re-init from panel.info on next render
};

// ─────────────────────────────────────────────────────────────────────────────

enum class AppState { Connect, Running };

struct CameraPanel {
    int32_t                     camera_id{-1};
    std::unique_ptr<CameraFeed> feed;
    // Double-buffered textures: render from tex_front, DMA into tex_back.
    // GPU copy engine (DMA) and graphics engine (render) run in parallel.
    uint32_t                    texture{0};      // tex_front — rendered each frame
    uint32_t                    tex_back{0};     // tex_back  — DMA target this frame
    int                         tex_w{0};        // front texture allocated dimensions
    int                         tex_h{0};
    int                         tex_ch{0};
    int                         tex_back_w{0};   // back texture allocated dimensions
    int                         tex_back_h{0};
    int                         tex_back_ch{0};
    bool                        tex_back_ready{false}; // DMA queued to tex_back; swap at next frame start
    void*                       dma_fence{nullptr};    // GLsync — signals when tex_back DMA completes

    // GPU debayer: raw Bayer (tex_front) → GLSL shader → debayer_tex → ImGui::Image
    uint32_t                    debayer_tex{0};    // GL_RGB8 output of debayer pass
    uint32_t                    fbo{0};            // FBO for debayer pass
    int                         debayer_tex_w{0};  // allocated dimensions of debayer_tex
    int                         debayer_tex_h{0};
    int                         bayer_r_col{0};    // R-pixel col offset in 2×2 cell (from PixelColorFilter)
    int                         bayer_r_row{0};    // R-pixel row offset

    uint32_t                    pbo[2]{0, 0};   // double-buffered PBOs — pre-allocated, never orphaned
    int                         pbo_idx{0};     // which PBO the CPU writes this frame
    bool                        pbo_ready{false}; // false until first write; upload_idx trails write_idx by 1
    int64_t                     pbo_sz{0};      // currently allocated byte size (0 = not yet allocated)
    int64_t                     last_ts_us{0};
    float                       transfer_ms{0.f};  // SHM→PBO memcpy wall-clock time
    GigeFrame                   fallback_frame_{};  // persistent buffer for non-admin gRPC fallback
    GigeCameraInfo              info{};
    ParamEditState              params{};
};

// ─────────────────────────────────────────────────────────────────────────────

class LiveViewApp {
public:
    LiveViewApp();
    ~LiveViewApp();

    // Called once per render frame.  delta_time_s == ImGuiIO::DeltaTime.
    void Render(float delta_time_s);

    bool WantsQuit() const { return wants_quit_; }

private:
    // ── Connection ────────────────────────────────────────────────────────────
    void RenderConnectDialog();
    void Connect(const std::string& addr);
    void Disconnect();

    // ── Main view ─────────────────────────────────────────────────────────────
    void RenderMainView(float delta_time_s);
    void RenderImagePanel(CameraPanel& panel, float avail_w, float avail_h);
    void RenderSidebar(CameraPanel& panel);
    void RenderInfoTab(CameraPanel& panel);
    void RenderParamTab(CameraPanel& panel);

    // ── Parameter helpers ─────────────────────────────────────────────────────
    void LoadParamsFromInfo(CameraPanel& panel);
    void ApplyLiveParams(CameraPanel& panel);
    void ApplyStopRequiredParams(CameraPanel& panel);

    // ── GL helpers ────────────────────────────────────────────────────────────
    void UploadTexture(CameraPanel& panel,
                       const uint8_t* data,
                       int w, int h, int channels);

    // ── GPU debayer ───────────────────────────────────────────────────────────
    void InitDebayerShader();
    void RunDebayerPass(CameraPanel& panel);

    // ── State ─────────────────────────────────────────────────────────────────
    AppState    state_{AppState::Connect};
    char        addr_buf_[256]{"localhost:50051"};
    std::string connect_error_;
    bool        wants_quit_{false};

    std::unique_ptr<GigECamera> cam_;
    GigeSystemState             sys_state_{};
    std::vector<CameraPanel>    panels_;

    int  selected_cam_idx_{0};
    bool sidebar_open_{true};

    // Debayer shader (compiled once at first Connect; destroyed at Disconnect)
    uint32_t debayer_prog_{0};
    uint32_t quad_vao_{0};
    uint32_t quad_vbo_{0};
    int      u_bayer_loc_{-1};
    int      u_r_col_loc_{-1};
    int      u_r_row_loc_{-1};

    float info_refresh_timer_{0.f};
    static constexpr float kInfoRefreshSecs = 2.f;
    static constexpr float kSidebarWidth    = 370.f;
};
