#include "RtpPusher.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>

#pragma comment(lib, "Ws2_32.lib")

static constexpr size_t   MAX_RTP_PAYLOAD = 1400;
static constexpr uint8_t  RTP_PT_H264     = 96;
static constexpr uint32_t RTP_CLOCK_RATE  = 90000;

// ─────────────────────────────────────────────────────────────────────────────
// RtpPushStreamer
// ─────────────────────────────────────────────────────────────────────────────

bool RtpPushStreamer::Start(FrameGrabber& grabber, H264Encoder& encoder,
                             const std::string& host, uint16_t port, int fps) {

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    udp_sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock_ == INVALID_SOCKET) {
        std::cerr << "[RtpPushStreamer] Failed to create UDP socket.\n";
        return false;
    }

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = 0;
    bind(udp_sock_, (sockaddr*)&local, sizeof(local));

    dest_.sin_family = AF_INET;
    if (inet_pton(AF_INET, host.c_str(), &dest_.sin_addr) <= 0) {
        std::cerr << "[RtpPushStreamer] Invalid destination host: " << host << "\n";
        closesocket(udp_sock_);
        udp_sock_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    dest_.sin_port = htons(port);

    std::mt19937 rng(std::random_device{}());
    ssrc_ = rng();

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&RtpPushStreamer::Loop, this, &grabber, &encoder, fps);

    std::cout << "[RtpPushStreamer] Pushing H.264/RTP to " << host << ":" << port << "\n";
    return true;
}

void RtpPushStreamer::Stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    if (udp_sock_ != INVALID_SOCKET) {
        closesocket(udp_sock_);
        udp_sock_ = INVALID_SOCKET;
        WSACleanup();
    }
}

void RtpPushStreamer::Loop(FrameGrabber* grabber, H264Encoder* encoder, int fps) {
    using clock = std::chrono::steady_clock;

    const auto frame_duration = std::chrono::microseconds(1'000'000 / fps);
    auto next_tick = clock::now();

    int64_t  last_ts          = -1;
    auto     last_frame_wall  = clock::now();
    uint32_t frames_sent      = 0;
    auto     fps_window_start = clock::now();

    while (running_.load(std::memory_order_acquire)) {
        next_tick += frame_duration;
        std::this_thread::sleep_until(next_tick);

        std::vector<uint8_t> pixels;
        int w = 0, h = 0;
        int64_t ts = 0;
        if (!grabber->GetLatestFrame(pixels, w, h, ts)) continue;

        // Skip duplicate frames — naturally caps output to the camera's real FPS.
        if (ts == last_ts) continue;
        last_ts = ts;

        auto nalus = encoder->Encode(pixels.data(), w, h);
        if (nalus.empty()) continue;

        // Use actual elapsed wall-clock time for RTP timestamps so playback
        // speed stays correct even when camera fps != configured fps.
        const auto   now           = clock::now();
        const double elapsed_s     = std::chrono::duration<double>(now - last_frame_wall).count();
        const auto   elapsed_ticks = static_cast<uint32_t>(elapsed_s * RTP_CLOCK_RATE);
        rtp_timestamp_ += std::max(elapsed_ticks, 1u);
        last_frame_wall = now;

        for (size_t i = 0; i < nalus.size(); ++i)
            PacketizeNalu(nalus[i], rtp_timestamp_, i == nalus.size() - 1);

        // Print actual push fps every 5 seconds.
        ++frames_sent;
        const double window_s =
            std::chrono::duration<double>(now - fps_window_start).count();
        if (window_s >= 5.0) {
            std::cout << "[RtpPushStreamer] actual fps: " << std::fixed
                      << std::setprecision(1) << (frames_sent / window_s) << "\n";
            frames_sent      = 0;
            fps_window_start = now;
        }
    }
}

void RtpPushStreamer::PacketizeNalu(const NaluUnit& nalu, uint32_t rtp_ts, bool last_in_frame) {
    const uint8_t* data = nalu.data.data();
    const size_t   size = nalu.data.size();
    if (size == 0) return;

    if (size <= MAX_RTP_PAYLOAD) {
        SendRtpPacket(last_in_frame, rtp_ts, data, size);
    } else {
        const uint8_t fu_indicator = (data[0] & 0xE0u) | 28u;
        const uint8_t nal_type     = data[0] & 0x1Fu;
        size_t offset = 1;
        while (offset < size) {
            const size_t chunk    = std::min(MAX_RTP_PAYLOAD - 2, size - offset);
            const bool   is_first = (offset == 1);
            const bool   is_last  = (offset + chunk >= size);
            uint8_t fu_header = nal_type;
            if (is_first) fu_header |= 0x80u;
            if (is_last)  fu_header |= 0x40u;
            std::vector<uint8_t> payload;
            payload.reserve(2 + chunk);
            payload.push_back(fu_indicator);
            payload.push_back(fu_header);
            payload.insert(payload.end(), data + offset, data + offset + chunk);
            SendRtpPacket(is_last && last_in_frame, rtp_ts, payload.data(), payload.size());
            offset += chunk;
        }
    }
}

void RtpPushStreamer::SendRtpPacket(bool marker, uint32_t ts,
                                     const uint8_t* payload, size_t len) {
    uint8_t hdr[12];
    hdr[0]  = 0x80;
    hdr[1]  = RTP_PT_H264 | (marker ? 0x80u : 0u);
    hdr[2]  = (rtp_seq_ >> 8) & 0xFF;
    hdr[3]  =  rtp_seq_       & 0xFF;
    hdr[4]  = (ts >> 24) & 0xFF;
    hdr[5]  = (ts >> 16) & 0xFF;
    hdr[6]  = (ts >>  8) & 0xFF;
    hdr[7]  =  ts        & 0xFF;
    hdr[8]  = (ssrc_ >> 24) & 0xFF;
    hdr[9]  = (ssrc_ >> 16) & 0xFF;
    hdr[10] = (ssrc_ >>  8) & 0xFF;
    hdr[11] =  ssrc_        & 0xFF;
    ++rtp_seq_;

    WSABUF bufs[2];
    bufs[0].buf = reinterpret_cast<char*>(hdr);
    bufs[0].len = 12;
    bufs[1].buf = const_cast<char*>(reinterpret_cast<const char*>(payload));
    bufs[1].len = static_cast<ULONG>(len);

    DWORD sent = 0;
    WSASendTo(udp_sock_, bufs, 2, &sent, 0,
              (const sockaddr*)&dest_, sizeof(dest_), nullptr, nullptr);
}
