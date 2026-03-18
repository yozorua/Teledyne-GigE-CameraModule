#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// ─────────────────────────────────────────────────────────────────────────────
// NaluUnit — one H.264 NAL unit (without Annex-B start code)
// ─────────────────────────────────────────────────────────────────────────────

struct NaluUnit {
    std::vector<uint8_t> data;
    uint8_t type{0};   // nal_unit_type (lower 5 bits of first byte)
};

// ─────────────────────────────────────────────────────────────────────────────
// H264Encoder
//
// Wraps libavcodec/libx264 for live H.264 encoding.
// Input:  RGB8 frames  (width * height * 3 bytes)
// Output: Annex-B NAL units, ready for RTP packetization
// ─────────────────────────────────────────────────────────────────────────────

class H264Encoder {
public:
    ~H264Encoder();

    H264Encoder(const H264Encoder&)            = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;
    H264Encoder() = default;

    /// @param bitrate_kbps  Target bitrate in kbps (default 4000).
    bool Init(int width, int height, int fps, int bitrate_kbps = 4000);

    /// Encode one RGB8 frame.
    /// Returns all NAL units produced (may be empty while encoder buffers).
    std::vector<NaluUnit> Encode(const uint8_t* rgb, int width, int height);

    bool HaveSPSPPS() const { return !sps_.empty() && !pps_.empty(); }

    // Raw SPS/PPS (no start code) — needed for SDP generation.
    const std::vector<uint8_t>& SPS() const { return sps_; }
    const std::vector<uint8_t>& PPS() const { return pps_; }

    /// "profile-level-id" hex string for SDP fmtp line (e.g. "42001f").
    std::string ProfileLevelId() const;

    /// "sprop-parameter-sets" base64 string for SDP fmtp line.
    std::string SpropParameterSets() const;

private:
    std::vector<NaluUnit> DrainPackets();
    void ExtractSPSPPS(const std::vector<NaluUnit>& nalus);

    AVCodecContext* ctx_{nullptr};
    AVFrame*        frame_{nullptr};
    AVPacket*       pkt_{nullptr};
    SwsContext*     sws_{nullptr};

    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;

    int width_{0};
    int height_{0};
};
