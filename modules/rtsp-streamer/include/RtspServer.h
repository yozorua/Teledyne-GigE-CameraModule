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
    std::string id;
    std::string client_addr;
    uint16_t    client_rtp_port{0};
    SOCKET      udp_sock{INVALID_SOCKET};
    sockaddr_in dest{};
    bool        playing{false};
    int32_t     stream_idx{-1};  // which CameraStream this session is watching

    uint16_t    rtp_seq{0};
    uint32_t    ssrc{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// CameraStream — one H.264 video track (one per camera)
// ─────────────────────────────────────────────────────────────────────────────

struct CameraStream {
    int32_t       camera_id{-1};
    std::string   path;           // RTSP URL path, e.g. "/cam0"
    FrameGrabber* grabber{nullptr};
    H264Encoder*  encoder{nullptr};
};

// ─────────────────────────────────────────────────────────────────────────────
// RtspServer
//
// RFC 2326 RTSP server with per-camera URL path routing.
// Supports: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER.
//
// Usage:
//   RtspServer server;
//   server.AddStream(0, grabber0, encoder0);  // camera 0 → /cam0
//   server.AddStream(1, grabber1, encoder1);  // camera 1 → /cam1
//   server.Start(8554, fps);                  // blocks until Stop()
// ─────────────────────────────────────────────────────────────────────────────

class RtspServer {
public:
    RtspServer()  = default;
    ~RtspServer() { Stop(); }

    RtspServer(const RtspServer&)            = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    // Register a camera before calling Start().
    // URL path is "/cam<camera_id>" (e.g. "/cam0").
    void AddStream(int32_t camera_id, FrameGrabber& grabber, H264Encoder& encoder);

    // Start the server. Blocks until Stop() is called from another thread.
    void Start(uint16_t port, int fps);

    void Stop();

private:
    // ── Threads ───────────────────────────────────────────────────────────────
    void AcceptLoop();
    void ClientLoop(SOCKET sock, std::string client_addr);
    void StreamLoop(int32_t stream_idx);  // one thread per camera stream

    // ── RTSP protocol ────────────────────────────────────────────────────────
    struct RtspRequest {
        std::string method;
        std::string url;
        std::string path;       // extracted from url
        int         cseq{0};
        std::string session_id;
        std::string transport;
        int         client_rtp{0};
        int         client_rtcp{0};
    };

    bool        ReadRequest(SOCKET sock, RtspRequest& req);
    std::string Response200(int cseq, const std::string& extra = {},
                            const std::string& body = {});
    std::string BuildSdp(int32_t stream_idx);
    int32_t     FindStreamByPath(const std::string& path) const;

    // ── RTP packetization (RFC 6184) ─────────────────────────────────────────
    void SendFrame(int32_t stream_idx, const std::vector<NaluUnit>& nalus, uint32_t rtp_ts);
    void PacketizeNalu(RtspSession& s, const NaluUnit& nalu,
                       uint32_t rtp_ts, bool last_in_frame);
    void SendRtpPacket(RtspSession& s, bool marker, uint32_t ts,
                       const uint8_t* payload, size_t len);

    // ── Session management ────────────────────────────────────────────────────
    RtspSession* FindSession(const std::string& id);
    void         RemoveSession(const std::string& id);

    std::vector<CameraStream> streams_;
    int                       fps_{30};

    SOCKET            listen_sock_{INVALID_SOCKET};
    std::atomic<bool> running_{false};

    std::thread              accept_thread_;
    std::vector<std::thread> stream_threads_;

    std::mutex               sessions_mutex_;
    std::vector<RtspSession> sessions_;

    std::vector<uint32_t> rtp_timestamps_;  // per-stream 90 kHz clock
    uint32_t              ts_increment_{3000};
    uint16_t              session_counter_{0};
};
