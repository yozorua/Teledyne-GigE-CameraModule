#include "RtspServer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#pragma comment(lib, "Ws2_32.lib")

static constexpr size_t   MAX_RTP_PAYLOAD = 1400;
static constexpr uint8_t  RTP_PT_H264     = 96;
static constexpr uint32_t RTP_CLOCK_RATE  = 90000;  // H.264 standard clock

// ─────────────────────────────────────────────────────────────────────────────
// Winsock init guard
// ─────────────────────────────────────────────────────────────────────────────

struct WsaGuard {
    WsaGuard()  { WSADATA w; WSAStartup(MAKEWORD(2,2), &w); }
    ~WsaGuard() { WSACleanup(); }
} g_wsa;

// ─────────────────────────────────────────────────────────────────────────────
// RTSP helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Read a line from a blocking socket (up to \r\n).
static bool RecvLine(SOCKET sock, std::string& line) {
    line.clear();
    char c;
    while (true) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') { if (!line.empty() && line.back() == '\r') line.pop_back(); return true; }
        line += c;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RtspServer — lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void RtspServer::Start(uint16_t port, FrameGrabber& grabber, H264Encoder& encoder, int fps) {
    grabber_      = &grabber;
    encoder_      = &encoder;
    fps_          = fps;
    ts_increment_ = RTP_CLOCK_RATE / static_cast<uint32_t>(fps);

    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) {
        std::cerr << "[RtspServer] socket() failed.\n";
        return;
    }

    int opt = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(listen_sock_, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(listen_sock_, 8) != 0) {
        std::cerr << "[RtspServer] bind/listen failed on port " << port << ".\n";
        closesocket(listen_sock_);
        return;
    }

    running_.store(true, std::memory_order_release);
    stream_thread_ = std::thread(&RtspServer::StreamLoop, this);
    accept_thread_ = std::thread(&RtspServer::AcceptLoop, this);

    std::cout << "[RtspServer] Listening on rtsp://0.0.0.0:" << port << "/stream\n";

    // Block until stopped.
    if (accept_thread_.joinable())  accept_thread_.join();
    if (stream_thread_.joinable()) stream_thread_.join();
}

void RtspServer::Stop() {
    running_.store(false, std::memory_order_release);
    if (listen_sock_ != INVALID_SOCKET) {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
    }
    {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        for (auto& s : sessions_) {
            if (s.udp_sock != INVALID_SOCKET) closesocket(s.udp_sock);
        }
        sessions_.clear();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// AcceptLoop — one thread per connected RTSP client
// ─────────────────────────────────────────────────────────────────────────────

void RtspServer::AcceptLoop() {
    while (running_.load(std::memory_order_acquire)) {
        sockaddr_in client_addr{};
        int len = sizeof(client_addr);
        SOCKET client = accept(listen_sock_, (sockaddr*)&client_addr, &len);
        if (client == INVALID_SOCKET) break;

        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        std::string addr_str = ip;

        std::cout << "[RtspServer] Client connected: " << addr_str << "\n";

        // Detach a handler thread for each client.
        std::thread([this, client, addr_str]() mutable {
            ClientLoop(client, addr_str);
        }).detach();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ClientLoop — handles RTSP signaling for one client
// ─────────────────────────────────────────────────────────────────────────────

bool RtspServer::ReadRequest(SOCKET sock, RtspRequest& req) {
    req = {};
    std::string line;

    // Request line: METHOD url RTSP/1.0
    if (!RecvLine(sock, line) || line.empty()) return false;
    std::istringstream rl(line);
    rl >> req.method >> req.url;

    // Headers
    while (RecvLine(sock, line) && !line.empty()) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key   = ToLower(Trim(line.substr(0, colon)));
        std::string value = Trim(line.substr(colon + 1));

        if (key == "cseq")      req.cseq       = std::stoi(value);
        if (key == "session")   req.session_id  = value.substr(0, value.find(';'));
        if (key == "transport") req.transport   = value;
    }

    // Parse client_port from Transport header (for SETUP)
    // e.g. "RTP/AVP;unicast;client_port=5000-5001"
    if (!req.transport.empty()) {
        size_t p = req.transport.find("client_port=");
        if (p != std::string::npos) {
            std::string ports = req.transport.substr(p + 12);
            req.client_rtp  = std::stoi(ports);
            size_t dash = ports.find('-');
            req.client_rtcp = (dash != std::string::npos)
                              ? std::stoi(ports.substr(dash + 1))
                              : req.client_rtp + 1;
        }
    }
    return true;
}

std::string RtspServer::Response200(int cseq, const std::string& extra, const std::string& body) {
    std::string resp = "RTSP/1.0 200 OK\r\nCSeq: " + std::to_string(cseq) + "\r\n";
    resp += extra;
    if (!body.empty()) {
        resp += "Content-Type: application/sdp\r\n";
        resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    resp += "\r\n";
    resp += body;
    return resp;
}

std::string RtspServer::BuildSdp() {
    // Fallback values if SPS/PPS not ready yet (shouldn't happen after Init primes encoder).
    std::string profile = encoder_->ProfileLevelId().empty() ? "42001f" : encoder_->ProfileLevelId();
    std::string sprop   = encoder_->SpropParameterSets();

    std::string sdp;
    sdp += "v=0\r\n";
    sdp += "o=- 0 0 IN IP4 0.0.0.0\r\n";
    sdp += "s=GigE Camera Stream\r\n";
    sdp += "c=IN IP4 0.0.0.0\r\n";
    sdp += "t=0 0\r\n";
    sdp += "m=video 0 RTP/AVP 96\r\n";
    sdp += "a=rtpmap:96 H264/90000\r\n";
    sdp += "a=fmtp:96 packetization-mode=1;profile-level-id=" + profile;
    if (!sprop.empty()) sdp += ";sprop-parameter-sets=" + sprop;
    sdp += "\r\n";
    sdp += "a=control:streamid=0\r\n";
    return sdp;
}

void RtspServer::ClientLoop(SOCKET sock, std::string client_addr) {
    std::mt19937 rng(std::random_device{}());
    std::string session_id;
    bool registered = false;

    while (running_.load(std::memory_order_acquire)) {
        RtspRequest req;
        if (!ReadRequest(sock, req)) break;

        std::cout << "[RtspServer] " << req.method << " from " << client_addr << "\n";

        if (req.method == "OPTIONS") {
            std::string resp = Response200(req.cseq,
                "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n");
            send(sock, resp.c_str(), (int)resp.size(), 0);

        } else if (req.method == "DESCRIBE") {
            std::string sdp  = BuildSdp();
            std::string resp = Response200(req.cseq, {}, sdp);
            send(sock, resp.c_str(), (int)resp.size(), 0);

        } else if (req.method == "SETUP") {
            // Assign session ID if new.
            if (session_id.empty()) {
                std::ostringstream ss;
                ss << std::hex << rng();
                session_id = ss.str();
            }

            // Create a UDP socket for sending RTP to this client.
            SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            uint16_t server_rtp_port = 0;
            {
                sockaddr_in srv{};
                srv.sin_family      = AF_INET;
                srv.sin_addr.s_addr = INADDR_ANY;
                srv.sin_port        = 0;  // OS assigns ephemeral port
                bind(udp, (sockaddr*)&srv, sizeof(srv));
                int addrlen = sizeof(srv);
                getsockname(udp, (sockaddr*)&srv, &addrlen);
                server_rtp_port = ntohs(srv.sin_port);
            }

            // Build destination sockaddr.
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            inet_pton(AF_INET, client_addr.c_str(), &dest.sin_addr);
            dest.sin_port = htons(static_cast<uint16_t>(req.client_rtp));

            {
                std::lock_guard<std::mutex> lk(sessions_mutex_);
                RtspSession s;
                s.id              = session_id;
                s.client_addr     = client_addr;
                s.client_rtp_port = static_cast<uint16_t>(req.client_rtp);
                s.udp_sock        = udp;
                s.dest            = dest;
                s.ssrc            = rng();
                s.playing         = false;
                sessions_.push_back(std::move(s));
            }
            registered = true;

            std::string transport =
                "Transport: RTP/AVP;unicast;client_port=" +
                std::to_string(req.client_rtp) + "-" +
                std::to_string(req.client_rtp + 1) +
                ";server_port=" + std::to_string(server_rtp_port) + "-" +
                std::to_string(server_rtp_port + 1) +
                ";ssrc=" + session_id + "\r\n";
            std::string resp = Response200(req.cseq,
                transport + "Session: " + session_id + "\r\n");
            send(sock, resp.c_str(), (int)resp.size(), 0);

        } else if (req.method == "PLAY") {
            {
                std::lock_guard<std::mutex> lk(sessions_mutex_);
                if (auto* s = FindSession(session_id)) s->playing = true;
            }
            std::string resp = Response200(req.cseq,
                "Session: " + session_id + "\r\n"
                "Range: npt=0.000-\r\n"
                "RTP-Info: url=" + req.url + ";seq=0;rtptime=0\r\n");
            send(sock, resp.c_str(), (int)resp.size(), 0);

        } else if (req.method == "TEARDOWN") {
            {
                std::lock_guard<std::mutex> lk(sessions_mutex_);
                RemoveSession(session_id);
            }
            registered = false;
            std::string resp = Response200(req.cseq,
                "Session: " + session_id + "\r\n");
            send(sock, resp.c_str(), (int)resp.size(), 0);
            break;

        } else if (req.method == "GET_PARAMETER") {
            // Used as a keepalive ping by many clients.
            std::string resp = Response200(req.cseq,
                session_id.empty() ? "" : "Session: " + session_id + "\r\n");
            send(sock, resp.c_str(), (int)resp.size(), 0);

        } else {
            // Unknown method — return 405.
            std::string resp = "RTSP/1.0 405 Method Not Allowed\r\nCSeq: " +
                               std::to_string(req.cseq) + "\r\n\r\n";
            send(sock, resp.c_str(), (int)resp.size(), 0);
        }
    }

    // Cleanup on disconnect.
    if (registered) {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        RemoveSession(session_id);
    }
    closesocket(sock);
    std::cout << "[RtspServer] Client disconnected: " << client_addr << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// StreamLoop — grab → encode → RTP send
// ─────────────────────────────────────────────────────────────────────────────

void RtspServer::StreamLoop() {
    using clock = std::chrono::steady_clock;

    const auto frame_duration = std::chrono::microseconds(1'000'000 / fps_);
    auto next_tick = clock::now();

    while (running_.load(std::memory_order_acquire)) {
        next_tick += frame_duration;
        std::this_thread::sleep_until(next_tick);

        // Skip if no playing clients.
        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            bool any = false;
            for (const auto& s : sessions_) if (s.playing) { any = true; break; }
            if (!any) continue;
        }

        // Get the latest frame from the grabber.
        std::vector<uint8_t> pixels;
        int w = 0, h = 0;
        int64_t ts = 0;
        if (!grabber_->GetLatestFrame(pixels, w, h, ts)) continue;

        // Encode to H.264.
        auto nalus = encoder_->Encode(pixels.data(), w, h);
        if (nalus.empty()) continue;

        // Distribute RTP to all playing clients.
        rtp_timestamp_ += ts_increment_;
        SendFrame(nalus, rtp_timestamp_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RTP packetization (RFC 6184)
// ─────────────────────────────────────────────────────────────────────────────

void RtspServer::SendFrame(const std::vector<NaluUnit>& nalus, uint32_t rtp_ts) {
    std::lock_guard<std::mutex> lk(sessions_mutex_);
    for (auto& session : sessions_) {
        if (!session.playing) continue;
        for (size_t i = 0; i < nalus.size(); ++i) {
            bool last = (i == nalus.size() - 1);
            PacketizeNalu(session, nalus[i], rtp_ts, last);
        }
    }
}

void RtspServer::PacketizeNalu(RtspSession& s, const NaluUnit& nalu,
                                uint32_t rtp_ts, bool last_in_frame) {
    const uint8_t* data = nalu.data.data();
    const size_t   size = nalu.data.size();
    if (size == 0) return;

    if (size <= MAX_RTP_PAYLOAD) {
        // Single NALU packet — the NALU byte stream is the RTP payload.
        SendRtpPacket(s, last_in_frame, rtp_ts, data, size);
    } else {
        // FU-A fragmentation.
        const uint8_t fu_indicator = (data[0] & 0xE0u) | 28u;
        const uint8_t nal_type     = data[0] & 0x1Fu;
        size_t offset = 1;  // skip the original NAL header byte

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

            SendRtpPacket(s, is_last && last_in_frame, rtp_ts,
                          payload.data(), payload.size());
            offset += chunk;
        }
    }
}

void RtspServer::SendRtpPacket(RtspSession& s, bool marker, uint32_t ts,
                                const uint8_t* payload, size_t len) {
    uint8_t hdr[12];
    hdr[0]  = 0x80;
    hdr[1]  = RTP_PT_H264 | (marker ? 0x80u : 0u);
    hdr[2]  = (s.rtp_seq >> 8) & 0xFF;
    hdr[3]  =  s.rtp_seq       & 0xFF;
    hdr[4]  = (ts >> 24) & 0xFF;
    hdr[5]  = (ts >> 16) & 0xFF;
    hdr[6]  = (ts >>  8) & 0xFF;
    hdr[7]  =  ts        & 0xFF;
    hdr[8]  = (s.ssrc >> 24) & 0xFF;
    hdr[9]  = (s.ssrc >> 16) & 0xFF;
    hdr[10] = (s.ssrc >>  8) & 0xFF;
    hdr[11] =  s.ssrc        & 0xFF;
    ++s.rtp_seq;

    // Scatter-gather send via two-buffer trick.
    WSABUF bufs[2];
    bufs[0].buf = reinterpret_cast<char*>(hdr);
    bufs[0].len = 12;
    bufs[1].buf = const_cast<char*>(reinterpret_cast<const char*>(payload));
    bufs[1].len = static_cast<ULONG>(len);

    DWORD sent = 0;
    WSASendTo(s.udp_sock, bufs, 2, &sent, 0,
              (const sockaddr*)&s.dest, sizeof(s.dest), nullptr, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Session management
// ─────────────────────────────────────────────────────────────────────────────

RtspSession* RtspServer::FindSession(const std::string& id) {
    for (auto& s : sessions_) if (s.id == id) return &s;
    return nullptr;
}

void RtspServer::RemoveSession(const std::string& id) {
    auto it = std::find_if(sessions_.begin(), sessions_.end(),
                           [&](const RtspSession& s){ return s.id == id; });
    if (it != sessions_.end()) {
        if (it->udp_sock != INVALID_SOCKET) closesocket(it->udp_sock);
        sessions_.erase(it);
    }
}
