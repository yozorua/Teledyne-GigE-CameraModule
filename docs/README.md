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
  ├─ pool_size                  int32                always 20
  ├─ num_cameras                int32
  ├─ buffer_camera_id[20]       int32[]              which camera wrote each slot
  ├─ buffer_width[20]           int32[]              actual ROI width per slot
  ├─ buffer_height[20]          int32[]              actual ROI height per slot
  └─ reference_counts[20]       atomic<int32>[]      -1=writing, 0=free, N=readers

Offset 368                  Pixel data pool
  ├─ slot 0                     single_image_size bytes
  ├─ slot 1                     single_image_size bytes
  └─ ...  (20 slots total)
```

> **Important:** Use `buffer_width[idx]` and `buffer_height[idx]`, not
> `image_width`/`image_height`, to determine actual pixel dimensions.  They
> differ when the camera ROI has been changed at runtime.

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

Pass float nodes via `float_value`; integer nodes via `int_value`.

| `param_name` | Type | Description |
|---|---|---|
| `ExposureTime` | float | Exposure in microseconds |
| `Gain` | float | Gain in dB |
| `Gamma` | float | Gamma correction (typically 0.5–4.0) |
| `BlackLevel` | float | Black level offset |
| `AcquisitionFrameRate` | float | Target frame rate (FPS) |
| `Width` | int | ROI width (pixels) |
| `Height` | int | ROI height (pixels) |
| `OffsetX` | int | ROI horizontal offset |
| `OffsetY` | int | ROI vertical offset |
| `BinningHorizontal` | int | Horizontal binning factor |
| `BinningVertical` | int | Vertical binning factor |

---

## Examples

- [`cpp/camera_analyzer.cpp`](cpp/camera_analyzer.cpp) — C++17 consumer using gRPC + direct SHM access
- [`python/camera_client.py`](python/camera_client.py) — Python consumer using grpcio + ctypes SHM

---

## Generating gRPC Stubs

**C++** stubs are generated automatically at CMake build time.

**Python** stubs — run once before using the Python example:

```bat
cd python
generate_proto.bat
```

This creates `camera_service_pb2.py` and `camera_service_pb2_grpc.py` in the
`python/` directory.
