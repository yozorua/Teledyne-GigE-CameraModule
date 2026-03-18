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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// RtspSession — state for one connected RTSP client
// ─────────────────────────────────────────────────────────────────────────────

struct RtspSession {
    std::string id;                         // session identifier string
    std::string client_addr;               // remote IP
    uint16_t    client_rtp_port{0};        // client's RTP receive port (UDP)
    SOCKET      udp_sock{INVALID_SOCKET};  // our sending UDP socket
    sockaddr_in dest{};                    // pre-built destination sockaddr
    bool        playing{false};

    uint16_t    rtp_seq{0};
    uint32_t    ssrc{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// RtspServer
//
// Minimal RFC 2326 RTSP server with RFC 6184 H.264/RTP over UDP.
// Supports: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER.
// ─────────────────────────────────────────────────────────────────────────────

class RtspServer {
public:
    RtspServer()  = default;
    ~RtspServer() { Stop(); }

    RtspServer(const RtspServer&)            = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    /// Start the server.  Blocks until Stop() is called from another thread.
    /// @param port     TCP port to listen on (default 8554).
    /// @param grabber  Frame source.
    /// @param encoder  H.264 encoder (must already be Init()'d).
    /// @param fps      Target stream frame rate.
    void Start(uint16_t port, FrameGrabber& grabber, H264Encoder& encoder, int fps);

    void Stop();

private:
    // ── Threads ───────────────────────────────────────────────────────────────
    void AcceptLoop();
    void ClientLoop(SOCKET sock, std::string client_addr);
    void StreamLoop();

    // ── RTSP helpers ──────────────────────────────────────────────────────────
    struct RtspRequest {
        std::string method;
        std::string url;
        int         cseq{0};
        std::string session_id;
        std::string transport;      // raw Transport header value (for SETUP)
        int         client_rtp{0};  // parsed client RTP port from Transport
        int         client_rtcp{0};
    };

    bool        ReadRequest(SOCKET sock, RtspRequest& req);
    std::string Response200(int cseq, const std::string& extra_headers = {},
                            const std::string& body = {});
    std::string BuildSdp();

    // ── RTP helpers ───────────────────────────────────────────────────────────
    void SendFrame(const std::vector<NaluUnit>& nalus, uint32_t rtp_ts);
    void PacketizeNalu(RtspSession& s, const NaluUnit& nalu,
                       uint32_t rtp_ts, bool last_in_frame);
    void SendRtpPacket(RtspSession& s, bool marker, uint32_t ts,
                       const uint8_t* payload, size_t len);

    // ── Session management ────────────────────────────────────────────────────
    RtspSession* FindSession(const std::string& id);
    void         RemoveSession(const std::string& id);

    FrameGrabber* grabber_{nullptr};
    H264Encoder*  encoder_{nullptr};
    int           fps_{30};

    SOCKET            listen_sock_{INVALID_SOCKET};
    std::atomic<bool> running_{false};

    std::thread accept_thread_;
    std::thread stream_thread_;

    std::mutex               sessions_mutex_;
    std::vector<RtspSession> sessions_;

    uint32_t rtp_timestamp_{0};      // incremented per frame (90 kHz clock)
    uint32_t ts_increment_{3000};    // 90000 / fps
    uint16_t session_counter_{0};    // for unique session IDs
};
