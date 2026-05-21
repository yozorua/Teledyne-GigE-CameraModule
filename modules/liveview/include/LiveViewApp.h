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

    std::string apply_status;     // feedback shown after Apply
    bool        loaded{false};    // false → re-init from panel.info on next render
};

// ─────────────────────────────────────────────────────────────────────────────

enum class AppState { Connect, Running };

struct CameraPanel {
    int32_t                     camera_id{-1};
    std::unique_ptr<CameraFeed> feed;
    uint32_t                    texture{0};      // GLuint
    uint32_t                    pbo[2]{0, 0};   // double-buffered PBOs — pre-allocated, never orphaned
    int                         pbo_idx{0};     // which PBO the CPU writes this frame
    int64_t                     pbo_sz{0};      // currently allocated byte size (0 = not yet allocated)
    int                         tex_w{0};
    int                         tex_h{0};
    int                         tex_ch{0};      // channels last uploaded (triggers glTexImage2D on change)
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

    float info_refresh_timer_{0.f};
    static constexpr float kInfoRefreshSecs = 2.f;
    static constexpr float kSidebarWidth    = 370.f;
};
