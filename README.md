# Teledyne GigE Camera Module

A high-performance Windows process that interfaces with multiple GigE cameras via the **Spinnaker SDK**, maintains maximum frame rates, and distributes images to separate analyzer processes on the same machine via **Windows Shared Memory** + **gRPC signalling** — without ever blocking the camera acquisition loop.

| Transport | What it carries | When to use |
|---|---|---|
| **gRPC** | Commands, status, frame metadata | Control, health checks, triggering saves |
| **Shared Memory** | Raw pixel data | High-frequency frame consumption (low latency) |

---

## Prerequisites

| Requirement | Notes |
|---|---|
| Windows 11 (64-bit) | Required — uses `Global\` shared memory namespace |
| MSVC 2022 / 2026 Build Tools | C++17, `cl.exe` on PATH |
| CMake ≥ 3.21 | [cmake.org](https://cmake.org/download/) |
| Ninja | `winget install Ninja-build.Ninja` |
| [vcpkg](https://github.com/microsoft/vcpkg) | For gRPC + Protobuf |
| Spinnaker SDK | Installed at `C:\Program Files\Teledyne\Spinnaker` |

### Install vcpkg dependencies

```cmd
vcpkg install grpc protobuf --triplet x64-windows
```

---

## Build

**Important:** Visual Studio `.sln`/`.vcxproj` generators are explicitly blocked. Use **Ninja**.

The easiest way is the provided PowerShell script, which sources the MSVC x64 environment automatically:

```powershell
powershell -ExecutionPolicy Bypass -File build_camera.ps1
```

The script (located at the repo root) configures and builds both executables in one step. Outputs are written to `build_camera.log` on the Desktop.

### Manual build (first time)

Open a **VS 2026 x64 Developer PowerShell** (or source `vcvars64.bat` manually), then:

```powershell
$VCPKG = "C:\Users\<you>\vcpkg\scripts\buildsystems\vcpkg.cmake"

# Configure
cmake -G "Ninja" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -B build .

# Build
cmake --build build --parallel
```

If the Spinnaker SDK is at a non-default path, add `-DSPINNAKER_DIR="D:/SDKs/Spinnaker"` to the configure step.

### Incremental rebuild

After changing source files, only the build step is needed — no reconfigure:

```powershell
cmake --build build --parallel
```

### Debug build

```powershell
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -B build-debug .

cmake --build build-debug --parallel
```

### Build outputs

| File | Description |
|---|---|
| `build\GigECameraModule.exe` | Camera server |
| `build\GigEDebugClient.exe` | Debug REPL |
| `build\*.dll` | vcpkg runtime DLLs (copied automatically by `VCPKG_APPLOCAL_DEPS`) |
| `dist\` | Self-contained deployment folder (exe + all DLLs) |

`cmake --install build --prefix dist` (run by `build_camera.ps1` automatically) produces the `dist\` folder.

---

## Deployment

Copy the entire `dist\` folder to the target machine. It is self-contained — no vcpkg or Visual Studio installation required.

```
dist\
  GigECameraModule.exe       Camera server (requires Administrator)
  GigEDebugClient.exe        gRPC REPL (no admin, no cameras needed)
  GigERtspServer.exe         RTSP stream server

  — gRPC / protobuf —
  abseil_dll.dll
  cares.dll
  legacy.dll
  libcrypto-3-x64.dll
  libprotobuf.dll
  libssl-3-x64.dll
  re2.dll
  zlib1.dll

  — FFmpeg (H.264 encoder, used by GigERtspServer) —
  avcodec-62.dll
  avutil-60.dll
  swscale-9.dll
  swresample-6.dll
  libx264-164.dll

  — Spinnaker runtime (used by GigECameraModule) —
  Spinnaker_v140.dll
  libiomp5md.dll

  — MSVC C++ runtime —
  msvcp140.dll  msvcp140_1.dll  msvcp140_2.dll  msvcp140_atomic_wait.dll
  vcruntime140.dll  vcruntime140_1.dll
```

### ⚠ Spinnaker SDK driver required on the camera machine

The `dist\` folder includes the Spinnaker **runtime DLLs** (`Spinnaker_v140.dll`, `libiomp5md.dll`), so you do **not** need to install the full Spinnaker SDK on the target machine.

However, `GigECameraModule.exe` **does** require the Spinnaker **GigE Vision network filter driver** to enumerate cameras over the network. This driver is only installed by the Spinnaker SDK installer — the DLLs alone are not sufficient.

**Install the driver on any machine that runs `GigECameraModule.exe`:**

1. Download the Spinnaker SDK installer from [Teledyne FLIR](https://www.flir.com/products/spinnaker-sdk/)
2. Run the installer and select **"GigE Vision Filter Driver"** (you may deselect the SDK headers, examples, and Python bindings if you only need the driver)
3. Reboot if prompted
4. Run `GigECameraModule.exe` as Administrator

> `GigEDebugClient.exe` and `GigERtspServer.exe` have **no Spinnaker dependency** and work on any machine with network access to the server — no driver or SDK installation needed.

---

## Run

The camera module **must be run as Administrator** — the `Global\` shared memory namespace requires `SeCreateGlobalPrivilege`.

```cmd
:: Default: gRPC on 0.0.0.0:50051, save frames to current directory
build\GigECameraModule.exe

:: Custom gRPC address and save directory
build\GigECameraModule.exe 0.0.0.0:50051 C:\frames
```

Stop with `Ctrl+C` — the process performs an orderly shutdown (stops acquisition, drains the disk-save queue, frees shared memory).

---

## Testing with the Debug Client

`GigEDebugClient.exe` does not require cameras or Administrator rights. Use it to exercise every gRPC endpoint interactively.

```cmd
build\GigEDebugClient.exe localhost:50051
```

### Typical test sequence

```
:: 1. Check module is reachable
camera> health

:: 2. List cameras and confirm model / IP / settings
camera> cameras

:: 3. Start acquisition on all cameras
camera> start

:: 4. Verify a frame arrives from each camera
camera> grab -1        (any camera)
camera> grab 0         (camera 0 only)
camera> grab 1         (camera 1 only)

:: 5. Inspect the shared memory pool directly (no gRPC round-trip)
camera> shm

:: 6. Adjust a parameter on all cameras
camera> set ExposureTime 5000.0 0
camera> set Gain 6.0 0

:: 7. Adjust a parameter on camera 0 only
camera> set 0 ExposureTime 8000.0 0

:: 8. Change the save directory and trigger a disk write
camera> savedir C:\frames
camera> save

:: 9. Hold a buffer open and release it manually
camera> grab 0 keep
  shm_index : 3
  ...
camera> release 3

:: 10. Stop camera 1 independently, leave camera 0 running
camera> stop 1
camera> state        (shows PARTIAL)

:: 11. Full stop and exit
camera> stop
camera> quit
```

### What `grab` prints

```
  shm_index : 4
  camera_id : 0
  size      : 4096x3000
  timestamp : 1741234567890123 us
  [SHM] buffer[4] — 12288000 bytes  |  min=12  max=247  mean=128.4
  [SHM] Pixel sample (5x5 grid across 4096x3000):
          128  131  129  133  130
          ...
  OK — Frame released.
```

`inspect <idx>` and `shm` read shared memory directly and work even when the gRPC server is not reachable, as long as `GigECameraModule.exe` is running.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                   GigECameraModule.exe (Producer)                │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │ SpinnakerCameraManager                                   │    │
│  │  • 1 acquisition thread per camera                       │    │
│  │  • Grabs ImagePtr → deep-copies into SharedMemory pool   │    │
│  │  • Releases ImagePtr immediately (non-blocking SDK)      │    │
│  │  • Disk-save queue (background thread, std::condition_variable)│
│  └─────────────────────────────────────────────────────────┘    │
│                          │ writes                                │
│  ┌────────────────────────▼────────────────────────────────┐    │
│  │ SharedMemoryManager  "Global\CameraImageBufferPool"      │    │
│  │  • Header  (SharedMemoryHeader + atomic refcounts)       │    │
│  │  • 20 × image buffers (contiguous, 5 per MAX_CAMERAS=4)  │    │
│  └────────────────────────┬────────────────────────────────┘    │
│                           │ index + metadata via gRPC            │
│  ┌────────────────────────▼────────────────────────────────┐    │
│  │ GrpcServer  (CameraControl service, port 50051)          │    │
│  │  GetLatestImageFrame()  → increment refcount, return idx │    │
│  │  ReleaseImageFrame()    → decrement refcount             │    │
│  └─────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────┘
                         ▲  gRPC call          ▲  mmap same SHM block
                         │                     │
              ┌──────────┴──────────────────────┴──────┐
              │       Analyzer Process(es)  (Consumer)  │
              │  1. GetLatestImageFrame() → idx         │
              │  2. OpenFileMapping("Global\Camera…")   │
              │  3. Read image data at buffer[idx]      │
              │  4. ReleaseImageFrame(idx)              │
              └─────────────────────────────────────────┘
```

### Shared Memory Buffer Pool

The pool header lives at offset 0 of the mapped region:

```
Offset 0                    SharedMemoryHeader (368 bytes)
  ├─ latest_buffer_index        atomic<int32>        most recent buffer, any camera
  ├─ latest_buffer_per_camera   atomic<int32>[4]     per-camera latest
  ├─ image_width / height       int32                SHM max allocation size
  ├─ image_channels             int32                always 1 (Mono8) unless changed
  ├─ single_image_size          size_t               bytes per buffer slot
  ├─ pool_size                  int32                always 32
  ├─ num_cameras                int32
  ├─ buffer_camera_id[32]       int32[]              which camera wrote each slot
  ├─ buffer_width[32]           int32[]              actual ROI width per slot
  ├─ buffer_height[32]          int32[]              actual ROI height per slot
  ├─ buffer_channels[32]        int32[]              1=raw Bayer, 3=BGR8/RGB8
  ├─ buffer_timestamp_us[32]    int64[]              system_clock capture time (µs since epoch)
  └─ reference_counts[32]       atomic<int32>[]      -1=writing, 0=free, N=readers

Offset 368                  Pixel data pool
  ├─ slot 0                     single_image_size bytes
  ├─ slot 1                     single_image_size bytes
  └─ ...  (32 slots total, 8 per camera)
```

> **Important:** Use `buffer_width[idx]` and `buffer_height[idx]`, not `image_width`/`image_height`, to determine actual pixel dimensions — they differ when the camera ROI has been changed at runtime.

Buffer state is tracked by `reference_counts[i]` (atomic int32):

| Value | Meaning |
|---|---|
| `-1` | Producer is currently writing (SHM_WRITING sentinel) |
| `0` | Free — available for producer to claim |
| `N > 0` | N consumer processes are reading |

The producer uses CAS `0 → -1` to claim a buffer, copies the image, then stores `0` and updates `latest_buffer_index` (both with `memory_order_release`).  Consumers use CAS `N → N+1` to pin a buffer before reading.

### GigE Bandwidth Management

The module targets **10 GigE** (10 Gbit/s = 1 250 MB/s) links.  On startup it applies two settings to every camera:

| GenICam node | Value | Reason |
|---|---|---|
| `GevSCPSPacketSize` | 9 000 bytes | Jumbo frames — reduces per-packet CPU overhead at high frame rates |
| `DeviceLinkThroughputLimit` | `1 250 000 000 ÷ camera_count` bytes/s | Divides the 10 Gbit/s budget equally so all cameras share the link without overrunning the NIC |

For a single camera the full 1 250 MB/s is available; with two cameras each gets 625 MB/s, and so on.

> **Using a 1 GigE NIC instead?**  Change the constant `GIGE_MAX_BANDWIDTH_BPS` in `src/SpinnakerCameraManager.cpp` from `1'250'000'000` to `125'000'000` and rebuild.

---

## gRPC API

See `proto/camera_service.proto`.  Package name: `camaramodule`.

### RPC reference

| RPC | Request message | Response | Description |
|---|---|---|---|
| `GetSystemState` | `Empty` | `SystemState` | Status string (`IDLE`/`ACQUIRING`/`PARTIAL`/`ERROR`), camera count, aggregate FPS |
| `StartAcquisition` | `CameraRequest` | `CommandStatus` | Start acquisition on one or all cameras |
| `StopAcquisition` | `CameraRequest` | `CommandStatus` | Stop acquisition on one or all cameras |
| `SetParameter` | `ParameterRequest` | `CommandStatus` | Set a GenICam node by name on one or all cameras |
| `TriggerDiskSave` | `Empty` | `CommandStatus` | Queue the next captured frame for disk write |
| `SetSaveDirectory` | `SaveDirectoryRequest` | `CommandStatus` | Change the directory frames are saved to at runtime |
| `GetCameraInfo` | `CameraRequest` | `CameraState` | Full live state for one camera (model, IP, ROI, binning, exposure, gain, FPS) |
| `GetLatestImageFrame` | `FrameRequest` | `FrameInfo` | Pin the latest buffer for a camera; returns SHM index + metadata (`timestamp` = µs since epoch at capture time) |
| `ReleaseImageFrame` | `ReleaseRequest` | `CommandStatus` | Decrement refcount on a pinned buffer |

### Per-camera targeting (`camera_id` field)

`StartAcquisition`, `StopAcquisition`, `SetParameter`, and `GetCameraInfo` all carry a `camera_id` field:

| `camera_id` | Behaviour |
|---|---|
| `-1` | All cameras (start/stop/set) or error (info) |
| `0` | Camera 0 only |
| `1` | Camera 1 only |
| … | … |

`GetLatestImageFrame` uses the same convention — pass `-1` to get the most recent frame from any camera.

> **Important:** consumers must call `ReleaseImageFrame` after every successful `GetLatestImageFrame` or the pool will exhaust.

### `CameraState` fields

`GetCameraInfo` returns the following live-read GenICam values:

| Field | GenICam node | Type | Notes |
|---|---|---|---|
| `model_name` | `DeviceModelName` | string | e.g. `Oryx ORX-10G-123S6C` |
| `serial` | `DeviceSerialNumber` | string | |
| `ip_address` | `GevCurrentIPAddress` | string | Dotted-decimal; empty if not GigE |
| `width` / `height` | `Width` / `Height` | int | Active ROI size in pixels |
| `offset_x` / `offset_y` | `OffsetX` / `OffsetY` | int | ROI top-left corner |
| `binning_h` / `binning_v` | `BinningHorizontal` / `BinningVertical` | int | 1 = no binning |
| `exposure_us` | `ExposureTime` | float | Microseconds |
| `exposure_auto` | `ExposureAuto` | string | `"Off"` / `"Once"` / `"Continuous"` |
| `gain_db` | `Gain` | float | dB |
| `gain_auto` | `GainAuto` | string | `"Off"` / `"Once"` / `"Continuous"` |
| `gamma` | `Gamma` | float | |
| `black_level` | `BlackLevel` | float | |
| `frame_rate` | `AcquisitionFrameRate` | float | 0 if node unavailable |
| `fps` | — | float | Computed from per-camera frame timestamps (last 30 frames) |
| `acquiring` | — | bool | Whether the acquisition thread is currently running |
| `ev_compensation` | `AutoExposureEVCompensation` | float | EV offset for auto-exposure; 0 if node unavailable |

### `SetParameter` — settable GenICam nodes

`SetParameter` accepts any writable GenICam node name. Node type is resolved in this order: enumeration (when `string_value` is non-empty) → float → integer.

**Enumeration nodes** — pass the entry name in `string_value`:

| Node name | Valid values | Notes |
|---|---|---|
| `ExposureAuto` | `"Off"` `"Once"` `"Continuous"` | Must be `"Off"` before writing `ExposureTime` |
| `GainAuto` | `"Off"` `"Once"` `"Continuous"` | Must be `"Off"` before writing `Gain` |
| `BalanceWhiteAuto` | `"Off"` `"Once"` `"Continuous"` | Color cameras only |
| `ChannelOrder` | `"BGR"` `"RGB"` | Software-only (no camera node). Default `"BGR"` — R↔B swap applied after debayer. Set `"RGB"` to skip the swap. Per-camera configurable. |
| `DebayerMode` | `"On"` `"Off"` | Software-only. Default `"On"` — debayer to BGR8/RGB8 before writing to SHM. Set `"Off"` to write raw Bayer bytes (1 channel) directly to SHM. Saved frames use `.raw` extension when off. Per-camera configurable. |

**Float nodes** — pass value in `float_value`:

| Node name | Notes |
|---|---|
| `ExposureTime` | Microseconds; requires `ExposureAuto = "Off"` |
| `Gain` | dB; requires `GainAuto = "Off"` |
| `Gamma` | Typically 0.25–4.0 |
| `BlackLevel` | |
| `AcquisitionFrameRate` | Requires `AcquisitionFrameRateEnable = true` |
| `AutoExposureEVCompensation` | EV offset applied on top of the auto-exposure algorithm (e.g. `-1.5` to `+1.5`). Requires `ExposureAuto = "Once"` or `"Continuous"`. Matches the **EV** slider in SpinView. |

**Integer nodes** — pass value in `int_value`:

| Node name | Notes |
|---|---|
| `Width` / `Height` | ROI size; must stop acquisition first on most cameras |
| `OffsetX` / `OffsetY` | ROI top-left; must stop acquisition first |
| `BinningHorizontal` / `BinningVertical` | Also `BinningX` / `BinningY` on some models |
| `GevSCPSPacketSize` | Set to 9000 for jumbo frames (auto-applied on start) |
| `DeviceLinkThroughputLimit` | Bytes/s; auto-applied per camera on start |

ROI and binning nodes require acquisition to be stopped first. Float nodes (e.g. `ExposureTime`, `Gain`) can be changed live.

---

## Debug Client

`GigEDebugClient.exe` is a standalone gRPC REPL for testing without writing any analyzer code. It does **not** link Spinnaker and does not need to run on the camera machine.

```cmd
build\GigEDebugClient.exe localhost:50051
```

### Command reference

**Module-level**

| Command | Action |
|---|---|
| `health` | Ping with a 3 s deadline — prints `UP` / `DOWN` + status |
| `state` | `GetSystemState` — status, camera count, aggregate FPS |
| `start [cam_id]` | `StartAcquisition` — omit or pass `-1` for all cameras |
| `stop [cam_id]` | `StopAcquisition` — omit or pass `-1` for all cameras |
| `restart [cam_id]` | Stop then start (client-side; does not restart the process) |

**Camera info**

| Command | Action |
|---|---|
| `cameras` | `GetCameraInfo` for every camera — full state table |
| `info <cam_id>` | `GetCameraInfo` for one camera |

**Parameter control**

| Command | Action |
|---|---|
| `set <name> <float> <int> [string]` | `SetParameter` on all cameras |
| `set <cam_id> <name> <float> <int> [string]` | `SetParameter` on one camera |

The first token is treated as a `cam_id` if it parses as an integer, otherwise as the node name (all cameras). The optional `string` argument is for enumeration nodes — pass `0 0` as float/int placeholders when using it.

```
# Float nodes
set ExposureTime 5000.0 0       # all cameras, 5 ms
set 0 ExposureTime 8000.0 0     # camera 0 only, 8 ms
set -1 Gain 10.0 0              # all cameras, 10 dB

# Integer nodes
set 1 Width 0 2048              # camera 1, ROI width

# Enumeration nodes (auto exposure / auto gain)
set ExposureAuto 0 0 Continuous   # enable auto-exposure on all cameras
set GainAuto     0 0 Continuous   # enable auto-gain on all cameras
set ExposureAuto 0 0 Once         # auto-adjust once, then lock
set ExposureAuto 0 0 Off          # disable — required before writing ExposureTime
set 0 GainAuto   0 0 Off          # disable on camera 0 only

# Debayer mode (per-camera)
set DebayerMode 0 0 Off           # raw Bayer → SHM (1 ch), saves .raw
set DebayerMode 0 0 On            # debayer → BGR8 (default)
```

**Disk save**

| Command | Action |
|---|---|
| `save` | `TriggerDiskSave` — flags next frame for write |
| `savedir <path>` | `SetSaveDirectory` — change output path at runtime |

Saved files are named `frame_cam<N>_<timestamp_ms>.raw` (raw binary, width × height bytes for Mono8).

**Frame inspection**

| Command | Action |
|---|---|
| `grab [cam_id] [keep]` | `GetLatestImageFrame` + SHM pixel inspect + auto-release |
| `release <idx>` | `ReleaseImageFrame` — release a buffer held with `keep` |
| `shm` | Dump full SHM pool state (refcounts, per-camera latest pointers) |
| `inspect <idx>` | Pixel stats for buffer N (min/max/mean + 5×5 sample grid) — direct SHM read, no gRPC |

The `shm` and `inspect` commands open shared memory read-only (`OpenFileMapping`) — no Administrator rights required on the consumer side.

---

## Integration Examples

| Language | Guide | Wrapper |
|---|---|---|
| C++ | [`examples/cpp/README.md`](examples/cpp/README.md) | [`examples/cpp/gige_camera.h`](examples/cpp/gige_camera.h) |
| Python | [`examples/python/`](examples/python/) | [`examples/python/gige_camera.py`](examples/python/gige_camera.py) |

### Generating gRPC Stubs

**C++** stubs are generated automatically at CMake build time.

**Python** stubs — run once before using the Python example:

```bat
cd examples\python
generate_proto.bat
```

This creates `camera_service_pb2.py` and `camera_service_pb2_grpc.py` in the `examples/python/` directory.

---

## Adjusting Image Dimensions

The shared memory is pre-allocated with fixed dimensions (`DEFAULT_WIDTH` × `DEFAULT_HEIGHT` × `DEFAULT_CHANNELS` in `src/main.cpp`).  Change these constants if your cameras output images larger than 1920 × 1080 Mono8, then rebuild.
