#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "FrameGrabber.h"
#include "H264Encoder.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// RtpPushStreamer
//
// Pushes H.264/RTP frames to a fixed UDP destination (no RTSP negotiation).
// Suitable for receiving with VLC, FFmpeg, or any RTP-capable endpoint:
//
//   ffplay  rtp://0.0.0.0:<port>
//   vlc     rtp://@:<port>
//
// For multi-camera, camera N is sent to base_port + N*2 (RTP), + N*2+1 (RTCP).
// ─────────────────────────────────────────────────────────────────────────────

class RtpPushStreamer {
public:
    RtpPushStreamer() = default;
    ~RtpPushStreamer() { Stop(); }

    RtpPushStreamer(const RtpPushStreamer&)            = delete;
    RtpPushStreamer& operator=(const RtpPushStreamer&) = delete;

    // @param grabber       Frame source (must already be Start()'d).
    // @param encoder       H.264 encoder (must already be Init()'d).
    // @param host          Destination IP address.
    // @param port          Destination RTP port (RTCP on port+1).
    // @param fps           Target frame rate.
    bool Start(FrameGrabber& grabber, H264Encoder& encoder,
               const std::string& host, uint16_t port, int fps);

    void Stop();

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

private:
    void Loop(FrameGrabber* grabber, H264Encoder* encoder, int fps);
    void PacketizeNalu(const NaluUnit& nalu, uint32_t rtp_ts, bool last_in_frame);
    void SendRtpPacket(bool marker, uint32_t ts, const uint8_t* payload, size_t len);

    SOCKET      udp_sock_{INVALID_SOCKET};
    sockaddr_in dest_{};

    uint16_t    rtp_seq_{0};
    uint32_t    ssrc_{0};
    uint32_t    rtp_timestamp_{0};
    uint32_t    ts_increment_{3000};

    std::atomic<bool> running_{false};
    std::thread       thread_;
};
