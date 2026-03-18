#include "H264Encoder.h"

#include <cstring>
#include <iostream>
#include <sstream>
#include <iomanip>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string Base64Encode(const uint8_t* data, size_t size) {
    static const char B64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t b = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < size) b |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < size) b |= static_cast<uint32_t>(data[i + 2]);
        out += B64[(b >> 18) & 0x3F];
        out += B64[(b >> 12) & 0x3F];
        out += (i + 1 < size) ? B64[(b >>  6) & 0x3F] : '=';
        out += (i + 2 < size) ? B64[(b >>  0) & 0x3F] : '=';
    }
    return out;
}

/// Parse Annex-B byte stream into individual NAL units (start codes stripped).
static std::vector<std::vector<uint8_t>> SplitAnnexB(const uint8_t* data, size_t size) {
    std::vector<std::vector<uint8_t>> out;

    // Returns the position of the next 00 00 01 start code, or 'size' if none.
    auto find_sc = [&](size_t from) -> size_t {
        for (size_t i = from; i + 2 < size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
                return i;
        }
        return size;
    };

    size_t pos = find_sc(0);
    while (pos < size) {
        // Skip the start code.  find_sc always returns the position of
        // the "00 01" tail, so the NALU payload begins exactly 3 bytes later
        // regardless of whether the prefix is the 3-byte (00 00 01) or
        // 4-byte (00 00 00 01) variant.
        size_t nalu_start = pos + 3;

        size_t next = find_sc(nalu_start);

        // Strip trailing zero bytes that belong to the next start code.
        size_t nalu_end = next;
        while (nalu_end > nalu_start && data[nalu_end - 1] == 0) --nalu_end;

        if (nalu_end > nalu_start)
            out.push_back({data + nalu_start, data + nalu_end});

        pos = next;
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// H264Encoder
// ─────────────────────────────────────────────────────────────────────────────

H264Encoder::~H264Encoder() {
    if (sws_)   { sws_freeContext(sws_);        sws_   = nullptr; }
    if (frame_) { av_frame_free(&frame_);       frame_ = nullptr; }
    if (pkt_)   { av_packet_free(&pkt_);        pkt_   = nullptr; }
    if (ctx_)   { avcodec_free_context(&ctx_);  ctx_   = nullptr; }
}

bool H264Encoder::Init(int width, int height, int fps, int bitrate_kbps) {
    width_  = width;
    height_ = height;

    // ── Find encoder (prefer libx264, fall back to any H.264) ────────────────
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        std::cerr << "[H264Encoder] libx264 not found, trying generic H.264.\n";
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec) {
        std::cerr << "[H264Encoder] No H.264 encoder available.\n";
        return false;
    }

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;

    ctx_->width     = width;
    ctx_->height    = height;
    ctx_->time_base = {1, fps};
    ctx_->framerate = {fps, 1};
    ctx_->bit_rate  = static_cast<int64_t>(bitrate_kbps) * 1000;
    ctx_->gop_size  = fps;          // keyframe every second
    ctx_->max_b_frames = 0;         // no B-frames for low latency
    ctx_->pix_fmt   = AV_PIX_FMT_YUV420P;

    // Low-latency tuning for libx264
    av_opt_set(ctx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx_->priv_data, "tune",   "zerolatency", 0);
    // Repeat SPS/PPS before each IDR so late-joining clients can decode
    av_opt_set(ctx_->priv_data, "x264-params", "repeat-headers=1", 0);

    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        std::cerr << "[H264Encoder] avcodec_open2 failed.\n";
        return false;
    }

    // ── Allocate frame and packet ─────────────────────────────────────────────
    frame_ = av_frame_alloc();
    frame_->format = AV_PIX_FMT_YUV420P;
    frame_->width  = width;
    frame_->height = height;
    av_frame_get_buffer(frame_, 0);

    pkt_ = av_packet_alloc();

    // ── swscale: RGB24 → YUV420P ──────────────────────────────────────────────
    sws_ = sws_getContext(
        width, height, AV_PIX_FMT_RGB24,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) {
        std::cerr << "[H264Encoder] sws_getContext failed.\n";
        return false;
    }

    // ── Prime encoder with a black frame to extract SPS/PPS immediately ───────
    std::vector<uint8_t> black(static_cast<size_t>(width) * height * 3, 0);
    Encode(black.data(), width, height);

    std::cout << "[H264Encoder] Init OK  " << width << "x" << height
              << "  " << fps << "fps  " << bitrate_kbps << "kbps\n";
    if (HaveSPSPPS())
        std::cout << "[H264Encoder] SDP profile-level-id=" << ProfileLevelId()
                  << "  sprop=" << SpropParameterSets() << "\n";
    return true;
}

std::vector<NaluUnit> H264Encoder::Encode(const uint8_t* rgb, int width, int height) {
    av_frame_make_writable(frame_);

    const uint8_t* src[1] = { rgb };
    int src_stride[1]     = { width * 3 };
    sws_scale(sws_, src, src_stride, 0, height,
              frame_->data, frame_->linesize);

    frame_->pts = (ctx_->frame_num);

    if (avcodec_send_frame(ctx_, frame_) < 0)
        return {};

    return DrainPackets();
}

std::vector<NaluUnit> H264Encoder::DrainPackets() {
    std::vector<NaluUnit> out;

    while (true) {
        int ret = avcodec_receive_packet(ctx_, pkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        // Split the Annex-B packet into individual NAL units.
        auto raw_nalus = SplitAnnexB(pkt_->data, static_cast<size_t>(pkt_->size));
        for (auto& raw : raw_nalus) {
            NaluUnit nu;
            nu.data = std::move(raw);
            nu.type = nu.data.empty() ? 0 : (nu.data[0] & 0x1F);
            out.push_back(std::move(nu));
        }

        av_packet_unref(pkt_);
    }

    if (!out.empty() && !HaveSPSPPS())
        ExtractSPSPPS(out);

    return out;
}

void H264Encoder::ExtractSPSPPS(const std::vector<NaluUnit>& nalus) {
    for (const auto& nu : nalus) {
        if (nu.type == 7 && sps_.empty()) sps_ = nu.data;   // SPS
        if (nu.type == 8 && pps_.empty()) pps_ = nu.data;   // PPS
        if (HaveSPSPPS()) break;
    }
}

std::string H264Encoder::ProfileLevelId() const {
    if (sps_.size() < 4) return "42001f";
    std::ostringstream ss;
    ss << std::hex << std::setw(2) << std::setfill('0') << (int)sps_[1]
       << std::hex << std::setw(2) << std::setfill('0') << (int)sps_[2]
       << std::hex << std::setw(2) << std::setfill('0') << (int)sps_[3];
    return ss.str();
}

std::string H264Encoder::SpropParameterSets() const {
    if (!HaveSPSPPS()) return {};
    return Base64Encode(sps_.data(), sps_.size())
         + "," +
           Base64Encode(pps_.data(), pps_.size());
}
