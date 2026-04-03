# GigE RTSP Server

Streams one camera from `GigECameraModule` as an H.264 RTSP feed.
Run one instance per camera.

## Prerequisites

- `GigECameraModule.exe` running (as Administrator)
- FFmpeg with x264 installed via vcpkg:
  ```
  vcpkg install "ffmpeg[avcodec,swscale,x264]" --triplet x64-windows
  ```

## Build

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Output: `build\GigERtspServer.exe`

## Usage

```
GigERtspServer.exe [grpc_addr] [camera_id] [rtsp_port] [fps] [bitrate_kbps]
```

| Argument | Default | Description |
|---|---|---|
| `grpc_addr` | `localhost:50051` | gRPC address of GigECameraModule |
| `camera_id` | `0` | 0-based camera index to stream |
| `rtsp_port` | `8554` | TCP port clients connect to |
| `fps` | `30` | Target encode / stream frame rate |
| `bitrate_kbps` | `4000` | H.264 bitrate in kbps (4000 = 4 Mbps) |

All arguments are optional and positional — defaults apply if omitted.

## Examples

**Single camera (all defaults):**
```bat
GigERtspServer.exe
```
Stream available at `rtsp://localhost:8554/stream`

**Two cameras on the same machine:**
```bat
rem Terminal 1 — camera 0
GigERtspServer.exe localhost:50051 0 8554 30 4000

rem Terminal 2 — camera 1
GigERtspServer.exe localhost:50051 1 8555 30 4000
```

| Camera | Stream URL |
|---|---|
| 0 | `rtsp://localhost:8554/stream` |
| 1 | `rtsp://localhost:8555/stream` |

**Watch with VLC or ffplay:**
```bat
ffplay rtsp://localhost:8554/stream
vlc    rtsp://localhost:8555/stream
```

## Adjusting exposure and gain for the stream

The RTSP server streams whatever pixels `GigECameraModule` puts in shared memory — it does not control the camera itself. To adjust brightness, use the debug client or the C++ wrapper to set parameters on the running module **before or while streaming**.

**Enable auto-exposure / auto-gain (recommended for varying lighting):**
```bat
GigEDebugClient.exe localhost:50051

camera> set ExposureAuto 0 0 Continuous
camera> set GainAuto     0 0 Continuous
```
The camera will start adjusting within a few frames. Changes appear in the VLC/ffplay window immediately — no restart needed.

**Lock in a specific exposure (consistent/controlled lighting):**
```bat
camera> set ExposureAuto 0 0 Off
camera> set ExposureTime 8000.0 0    # 8 ms
camera> set GainAuto     0 0 Off
camera> set Gain         2.0 0       # 2 dB
```

**Check current values while streaming:**
```bat
camera> cameras
  exposure   : 6241.0 us  [auto: Continuous]
  gain       : 1.80 dB    [auto: Continuous]
```

---

## Architecture

```
GigECameraModule  ←──gRPC──  FrameGrabber thread
                  ←──SHM───  (pixels copied out each frame)
                                      │
                               H264Encoder (libx264)
                                      │
                              RtspServer (RFC 2326)
                              ├─ DESCRIBE → SDP
                              ├─ SETUP    → UDP socket per client
                              └─ PLAY     → RTP/UDP (RFC 6184)
```

Frames flow: `GigECameraModule SHM → RGB8 → YUV420P → H.264 Annex-B → RTP/UDP → client`
