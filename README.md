# Teledyne GigE Camera Module

A high-performance Windows process that interfaces with multiple GigE cameras via the **Spinnaker SDK**, maintains maximum frame rates, and distributes images to separate analyzer processes on the same machine via **Windows Shared Memory** + **gRPC signalling** — without ever blocking the camera acquisition loop.

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

Copy the entire `dist\` folder to the target machine. It contains both executables and all vcpkg runtime DLLs:

```
dist\
  GigECameraModule.exe
  GigEDebugClient.exe
  abseil_dll.dll
  cares.dll
  legacy.dll
  libcrypto-3-x64.dll
  libprotobuf-lite.dll
  libprotobuf.dll
  libssl-3-x64.dll
  re2.dll
  zlib1.dll
```

The target machine also needs the **Spinnaker SDK runtime** installed (for `GigECameraModule.exe` only). The SDK installer adds the Spinnaker DLLs (`Spinnaker_v140.dll`, etc.) to `PATH` automatically. `GigEDebugClient.exe` has no Spinnaker dependency and runs standalone.

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
:: 1. Check server is reachable and cameras are found
camera> state

:: 2. Start acquisition
camera> start

:: 3. Verify a frame arrives from each camera
camera> grab -1        (any camera)
camera> grab 0         (camera 0 only)
camera> grab 1         (camera 1 only)

:: 4. Inspect the shared memory pool directly (no gRPC round-trip)
camera> shm

:: 5. Adjust a camera parameter
camera> set ExposureTime 5000.0 0
camera> set Gain 6.0 0

:: 6. Trigger a frame save to disk
camera> save

:: 7. Hold a buffer open and release it manually
camera> grab 0 keep
  shm_index : 3
  ...
camera> release 3

:: 8. Stop acquisition and exit
camera> stop
camera> quit
```

### What `grab` prints

```
  shm_index : 4
  camera_id : 0
  size      : 1920×1080
  timestamp : 1741234567890 ms
  [SHM] 2073600 bytes read | min=12  max=247  mean=128.4
  [SHM] Pixel sample (5×5 grid across 1920×1080):
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
│  │  • 10 × image buffers (contiguous)                       │    │
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
[SharedMemoryHeader]          ← sizeof(SharedMemoryHeader)
[Buffer 0][Buffer 1]...[9]    ← 10 × (width × height × channels) bytes
```

Buffer state is tracked by `reference_counts[i]` (atomic int32):

| Value | Meaning |
|---|---|
| `-1` | Producer is currently writing (SHM_WRITING sentinel) |
| `0` | Free — available for producer to claim |
| `N > 0` | N consumer processes are reading |

The producer uses CAS `0 → -1` to claim a buffer, copies the image, then stores `0` and updates `latest_buffer_index` (both with `memory_order_release`).  Consumers use CAS `N → N+1` to pin a buffer before reading.

### GigE Bandwidth Management

- `DeviceLinkThroughputLimit` is set per camera to `125 MB/s ÷ camera_count`.
- `GevSCPSPacketSize` is set to 9 000 bytes (jumbo frames).

---

## gRPC API

See `proto/camera_service.proto`.  Package name: `camaramodule`.

| RPC | Request | Description |
|---|---|---|
| `GetSystemState` | `Empty` | Status string, camera count, current FPS |
| `StartAcquisition` | `Empty` | Begin frame capture on all cameras |
| `StopAcquisition` | `Empty` | Stop capture and join acquisition threads |
| `SetParameter` | `ParameterRequest` | Set a GenICam node by name (float or int) |
| `TriggerDiskSave` | `Empty` | Queue the next frame for disk write |
| `GetLatestImageFrame` | `FrameRequest` | Pin a buffer; returns index + metadata |
| `ReleaseImageFrame` | `ReleaseRequest` | Release a pinned buffer back to the pool |

### Multi-camera frame selection

`GetLatestImageFrame` takes a `FrameRequest { int32 camera_id }`:

| `camera_id` | Behaviour |
|---|---|
| `-1` | Latest frame from **any** camera (global latest) |
| `0` | Latest frame from camera 0 only |
| `1` | Latest frame from camera 1 only |

`FrameInfo` returns `camera_id` so the consumer knows which camera produced the buffer.

> **Important:** consumers must call `ReleaseImageFrame` after every successful `GetLatestImageFrame` or the pool will exhaust.

---

## Debug Client

`GigEDebugClient.exe` is a standalone gRPC REPL for testing without writing any analyzer code. It does **not** link Spinnaker and does not need to run on the camera machine.

```cmd
build\GigEDebugClient.exe localhost:50051
```

| Command | Action |
|---|---|
| `state` | `GetSystemState` |
| `start` / `stop` | Start/stop acquisition |
| `set ExposureTime 5000.0 0` | `SetParameter` |
| `save` | `TriggerDiskSave` |
| `grab` | Grab any-camera frame, inspect SHM pixels, auto-release |
| `grab 0` | Grab from camera 0 only |
| `grab 1 keep` | Grab from camera 1, hold buffer open |
| `release <idx>` | Manually release a held buffer |
| `shm` | Dump full SHM pool state (refcounts, per-camera latest) |
| `inspect <idx>` | Pixel stats for buffer N (min/max/mean + 5×5 sample grid) |

The `shm` and `inspect` commands read shared memory directly (read-only `OpenFileMapping` — no admin rights required on the consumer side).

---

## Adjusting Image Dimensions

The shared memory is pre-allocated with fixed dimensions (`DEFAULT_WIDTH` × `DEFAULT_HEIGHT` × `DEFAULT_CHANNELS` in `src/main.cpp`).  Change these constants if your cameras output images larger than 1920 × 1080 Mono8, then rebuild.
