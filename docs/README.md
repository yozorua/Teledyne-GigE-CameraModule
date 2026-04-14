# GigE Camera Module — Integration Guide

This guide shows downstream applications how to receive camera frames from the
`GigECameraModule` process over its two transport mechanisms:

| Transport | What it carries | When to use |
|---|---|---|
| **gRPC** | Commands, status, frame metadata | Control, health checks, triggering saves |
| **Shared Memory** | Raw pixel data | High-frequency frame consumption (low latency) |

---

## Prerequisites

The `GigECameraModule.exe` process must already be running:

```powershell
# Run as Administrator (required for Global\ SHM namespace)
.\GigECameraModule.exe [grpc_address] [save_dir]
# Defaults: 0.0.0.0:50051   C:\CameraImages
```

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│              GigECameraModule.exe  (producer)           │
│  Spinnaker SDK → SpinnakerCameraManager                 │
│       └─► SharedMemoryManager  ──► Global\CameraImageBufferPool │
│       └─► GrpcServer  ──────────► 0.0.0.0:50051        │
└─────────────────────────────────────────────────────────┘
          │ gRPC (metadata + index)       │ SHM (pixels)
          ▼                               ▼
┌─────────────────────────────────────────────────────────┐
│              Your Application  (consumer)               │
│  1. GetLatestImageFrame ──► returns SHM buffer index    │
│  2. Open "Global\CameraImageBufferPool" read-only       │
│  3. Copy pixel bytes at: header_size + index * slot_size│
│  4. ReleaseImageFrame  ──► producer can reuse buffer    │
└─────────────────────────────────────────────────────────┘
```

### Shared Memory Layout

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
  └─ reference_counts[32]       atomic<int32>[]      -1=writing, 0=free, N=readers

Offset 368                  Pixel data pool
  ├─ slot 0                     single_image_size bytes
  ├─ slot 1                     single_image_size bytes
  └─ ...  (32 slots total, 8 per camera)
```

> **Important:** Use `buffer_width[idx]` and `buffer_height[idx]`, not
> `image_width`/`image_height`, to determine actual pixel dimensions.  They
> differ when the camera ROI has been changed at runtime.

> **Slot partitioning:** The 32 slots are divided into 8 exclusive slots per
> camera — camera 0 uses slots 0–7, camera 1 uses slots 8–15, camera 2 uses
> slots 16–23, camera 3 uses slots 24–31.  A camera never writes outside its
> own range, so `latest_buffer_per_camera[N]` can only point to data produced
> by camera N.

---

## gRPC API Reference

Proto package: **`camaramodule`** (note spelling)

| RPC | Request | Response | Notes |
|---|---|---|---|
| `GetSystemState` | `Empty` | `SystemState` | IDLE / ACQUIRING / PARTIAL / ERROR |
| `StartAcquisition` | `CameraRequest` | `CommandStatus` | camera_id=-1 → all |
| `StopAcquisition` | `CameraRequest` | `CommandStatus` | camera_id=-1 → all |
| `SetParameter` | `ParameterRequest` | `CommandStatus` | camera_id=-1 → all |
| `TriggerDiskSave` | `CameraRequest` | `CommandStatus` | Saves next frame as JPEG |
| `SetSaveDirectory` | `SaveDirectoryRequest` | `CommandStatus` | Runtime path change |
| `GetCameraInfo` | `CameraRequest` | `CameraState` | Full camera state |
| `GetLatestImageFrame` | `FrameRequest` | `FrameInfo` | Pins buffer; must release |
| `ReleaseImageFrame` | `ReleaseRequest` | `CommandStatus` | Unpins buffer |

### Settable Parameters (`SetParameter`)

The server resolves node type in this order: enumeration (if `string_value` is non-empty) → float → integer.

**Enumeration nodes** — pass entry name via `string_value`:

| `param_name` | Valid values | Notes |
|---|---|---|
| `ExposureAuto` | `"Off"` `"Once"` `"Continuous"` | Must be `"Off"` before writing `ExposureTime` |
| `GainAuto` | `"Off"` `"Once"` `"Continuous"` | Must be `"Off"` before writing `Gain` |
| `BalanceWhiteAuto` | `"Off"` `"Once"` `"Continuous"` | Color cameras only |
| `ChannelOrder` | `"BGR"` `"RGB"` | Software-only (no camera node). Default `"BGR"` — R↔B swap applied after debayer. Set `"RGB"` to skip the swap. Per-camera configurable. |

**Float nodes** — pass value via `float_value`:

| `param_name` | Description |
|---|---|
| `ExposureTime` | Exposure in microseconds (requires `ExposureAuto = "Off"`) |
| `Gain` | Gain in dB (requires `GainAuto = "Off"`) |
| `Gamma` | Gamma correction (typically 0.5–4.0) |
| `BlackLevel` | Black level offset |
| `AcquisitionFrameRate` | Target frame rate (FPS) |

**Integer nodes** — pass value via `int_value`:

| `param_name` | Description |
|---|---|
| `Width` | ROI width (pixels) |
| `Height` | ROI height (pixels) |
| `OffsetX` | ROI horizontal offset |
| `OffsetY` | ROI vertical offset |
| `BinningHorizontal` | Horizontal binning factor |
| `BinningVertical` | Vertical binning factor |

---

## Examples

| Language | Guide | Wrapper |
|---|---|---|
| C++ | [`examples/cpp/README.md`](examples/cpp/README.md) | [`examples/cpp/gige_camera.h`](examples/cpp/gige_camera.h) |
| Python | [`examples/python/`](examples/python/) | [`examples/python/gige_camera.py`](examples/python/gige_camera.py) |

---

## Generating gRPC Stubs

**C++** stubs are generated automatically at CMake build time.

**Python** stubs — run once before using the Python example:

```bat
cd examples\python
generate_proto.bat
```

This creates `camera_service_pb2.py` and `camera_service_pb2_grpc.py` in the
`examples/python/` directory.
