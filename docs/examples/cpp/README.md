# GigE Camera C++ SDK

Single-header C++17 wrapper that hides all gRPC and shared-memory details.
Copy one file, add one CMake target — done.

```
GigECameraModule.exe  ←──gRPC (metadata)──  GigECamera::grab()
                      ←──SHM  (pixels)   ──  pixels copied into GigeFrame
                                              you own the data, no release needed
```

---

## Prerequisites

| Requirement | Notes |
|---|---|
| **GigECameraModule.exe** running as Administrator | The server process; must be started before your app |
| **vcpkg** with `grpc` and `protobuf` | `vcpkg install grpc protobuf --triplet x64-windows` |
| **CMake ≥ 3.21**, **Ninja**, **MSVC C++17** | The toolchain used by this project |

---

## Installation

### Step 1 — Copy the header

Copy `gige_camera.h` anywhere into your project. It has no compiled `.cpp` counterpart.

```
your_project/
├── src/
│   └── main.cpp
├── gige_camera.h          ← copy here (or any include path you prefer)
└── CMakeLists.txt
```

### Step 2 — Set up your CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.21)
project(MyApp CXX)
set(CMAKE_CXX_STANDARD 17)

# vcpkg toolchain must be supplied at configure time:
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#         -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_BUILD_TYPE=Release -B build .
find_package(gRPC     CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)

# Generate gRPC stubs from camera_service.proto.
# Adjust the path to camera_service.proto relative to your project.
set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PROTO_OUT}")

add_custom_command(
    OUTPUT  "${PROTO_OUT}/camera_service.pb.cc"
            "${PROTO_OUT}/camera_service.pb.h"
            "${PROTO_OUT}/camera_service.grpc.pb.cc"
            "${PROTO_OUT}/camera_service.grpc.pb.h"
    COMMAND $<TARGET_FILE:protobuf::protoc>
            "--cpp_out=${PROTO_OUT}"
            "--grpc_out=${PROTO_OUT}"
            "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
            "-I${CMAKE_CURRENT_SOURCE_DIR}/proto"
            "${CMAKE_CURRENT_SOURCE_DIR}/proto/camera_service.proto"
    DEPENDS protobuf::protoc gRPC::grpc_cpp_plugin)

add_executable(my_app
    src/main.cpp
    "${PROTO_OUT}/camera_service.pb.cc"
    "${PROTO_OUT}/camera_service.grpc.pb.cc")

target_include_directories(my_app PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"   # gige_camera.h
    "${PROTO_OUT}")                 # generated proto headers

target_link_libraries(my_app PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf)

target_compile_definitions(my_app PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
```

### Step 3 — Build

```powershell
# From a VS 2026 x64 Developer PowerShell (or after sourcing vcvars64.bat):
$VCPKG = "C:\Users\you\vcpkg\scripts\buildsystems\vcpkg.cmake"

cmake -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG `
      -DVCPKG_TARGET_TRIPLET=x64-windows `
      -B build .

cmake --build build --parallel
```

---

## Step-by-Step Usage

### 1. Include the header

```cpp
#include "gige_camera.h"
```

That is the only include you need. Everything — gRPC stubs, SHM reader, all types — lives inside it.

---

### 2. Connect to the server

```cpp
GigECamera cam("localhost:50051");   // default address
```

The constructor creates a gRPC channel; it does **not** block or throw even if the server is not yet running. The connection is established lazily on the first RPC call.

To use a remote host or custom port:
```cpp
GigECamera cam("192.168.1.100:50051");
```

---

### 3. Check system state

```cpp
GigeSystemState s = cam.state();
// s.status          — "IDLE" | "ACQUIRING" | "PARTIAL" | "ERROR"
// s.connected_cameras — number of detected cameras
// s.current_fps       — aggregate frame rate across all cameras

std::printf("Status: %s  cameras: %d  fps: %.1f\n",
            s.status.c_str(), s.connected_cameras, s.current_fps);
```

---

### 4. Read camera info

```cpp
std::optional<GigeCameraInfo> info = cam.info(0);   // camera 0
if (info) {
    std::printf("Model  : %s\n",    info->model_name.c_str());
    std::printf("Serial : %s\n",    info->serial.c_str());
    std::printf("IP     : %s\n",    info->ip_address.c_str());
    std::printf("Size   : %dx%d\n", info->width, info->height);
    std::printf("Exp    : %.0f us\n", info->exposure_us);
    std::printf("Gain   : %.2f dB\n", info->gain_db);
    std::printf("FPS    : %.1f\n",  info->fps);
}
```

> `info()` returns `std::nullopt` if the gRPC call fails (e.g. server not running).

---

### 5. Start acquisition

```cpp
cam.start();          // start all connected cameras
cam.start(0);         // start camera 0 only
cam.start(1);         // start camera 1 only
```

---

### 6. Grab a frame

#### Non-blocking grab (latest frame, return immediately)

```cpp
std::optional<GigeFrame> frame = cam.grab(0);   // camera 0
if (!frame) {
    // No frame available yet — camera may not have produced one yet.
    return;
}
// frame is valid here
```

#### Blocking grab (wait for a new unique frame)

Use `grab_wait()` in a processing loop to ensure you never process the same frame twice:

```cpp
int64_t last_ts = 0;

while (running) {
    auto frame = cam.grab_wait(
        0,      // camera_id
        last_ts,  // skip frames with this timestamp (0 = accept any)
        500     // timeout in ms — returns nullopt if no new frame in 500 ms
    );

    if (!frame) continue;   // timeout — loop again

    last_ts = frame->timestamp_ms;   // remember for next call
    // process frame ...
}
```

#### Grab from whichever camera produced the most recent frame

```cpp
auto frame = cam.grab_any();           // non-blocking
auto frame = cam.grab_any_wait(last_ts, 500);  // blocking
```

---

### 7. Access pixel data

`frame->pixels` is an **owned** `std::vector<uint8_t>`. Pixels are already copied from shared memory before `grab()` returns. You do not need to release anything.

```
Pixel layout: R G B R G B R G B ...   (interleaved, 3 bytes per pixel)
Total bytes : frame->width * frame->height * 3
```

```cpp
const uint8_t* data   = frame->pixels.data();
const int      width  = frame->width;
const int      height = frame->height;
const int      ch     = frame->channels;   // always 3

// Access pixel at (row, col):
int offset = (row * width + col) * 3;
uint8_t r = data[offset + 0];
uint8_t g = data[offset + 1];
uint8_t b = data[offset + 2];

// Metadata
int     cam_id = frame->camera_id;
int64_t ts_ms  = frame->timestamp_ms;
```

#### Wrap in an OpenCV Mat

```cpp
#include <opencv2/opencv.hpp>

cv::Mat rgb_mat(frame->height, frame->width, CV_8UC3,
                frame->pixels.data());

// OpenCV uses BGR order internally — convert if needed:
cv::Mat bgr_mat;
cv::cvtColor(rgb_mat, bgr_mat, cv::COLOR_RGB2BGR);

cv::imshow("Camera", bgr_mat);
cv::waitKey(1);
```

---

### 8. Stop acquisition

```cpp
cam.stop();      // stop all cameras
cam.stop(0);     // stop camera 0 only
```

---

## Camera Control

All parameter setters return `bool` — `true` on success.
Pass `camera_id = -1` (default) to apply the setting to all connected cameras simultaneously.

### Exposure

```cpp
cam.set_exposure(5000.f);          // 5 ms, all cameras
cam.set_exposure(10000.f, 0);      // 10 ms, camera 0 only
cam.set_exposure(3000.f, 1);       // 3 ms, camera 1 only
```

### Gain

```cpp
cam.set_gain(0.f);     // 0 dB (minimum)
cam.set_gain(6.f, 0);  // 6 dB on camera 0
```

### Gamma

```cpp
cam.set_gamma(1.0f);    // linear (default)
cam.set_gamma(0.7f, 0); // slight brightening on camera 0
```

### Frame rate

```cpp
cam.set_frame_rate(30.f);    // 30 fps, all cameras
cam.set_frame_rate(10.f, 1); // 10 fps, camera 1
```

### Auto exposure

```cpp
// Enable continuous auto-exposure (camera adjusts exposure automatically)
cam.set_exposure_auto("Continuous");

// Adjust once, then return to fixed exposure
cam.set_exposure_auto("Once");

// Disable auto-exposure — required before set_exposure() takes effect
cam.set_exposure_auto("Off");
cam.set_exposure(5000.f);   // now manual: 5 ms
```

### Auto gain

```cpp
cam.set_gain_auto("Continuous");   // continuously adjusts gain
cam.set_gain_auto("Once");         // adjust once, then lock
cam.set_gain_auto("Off");          // disable — required before set_gain()
cam.set_gain(3.f);                 // now manual: 3 dB
```

> **Note:** `ExposureAuto` and `GainAuto` must be set to `"Off"` before writing manual values via `set_exposure()` or `set_gain()`. Most cameras silently ignore manual writes while auto mode is active.

### Check current auto mode

Auto-mode state is included in `cam.info()`:

```cpp
auto info = cam.info(0);
if (info) {
    std::printf("ExposureAuto : %s\n", info->exposure_auto.c_str());  // "Off" / "Once" / "Continuous"
    std::printf("GainAuto     : %s\n", info->gain_auto.c_str());
}
```

### Region of Interest (ROI)

Always reset offsets to zero before changing dimensions — the helper does this automatically:

```cpp
// Full ROI reset example: 1280×720 centered on a 4096×2160 sensor
cam.set_roi(1280, 720, 1408, 720);   // width, height, offset_x, offset_y
// camera_id defaults to -1 (all cameras)

// Per-camera ROI:
cam.set_roi(1920, 1080, 0, 0, 0);   // camera 0 — top-left 1080p crop
cam.set_roi(1920, 1080, 0, 0, 1);   // camera 1 — same ROI
```

> After changing ROI, the `frame->width` and `frame->height` returned by `grab()` reflect the **actual** capture size.

### Any GenICam node

```cpp
// Float node
cam.set_param("BlackLevel", /*float_value=*/4.0f, /*int_value=*/0, /*camera_id=*/-1);

// Integer node
cam.set_param("BinningHorizontal", 0.f, 2, -1);   // 2× horizontal binning
```

---

## Save Images to Disk

Queue the next frame for JPEG save (written by the server process to its configured directory):

```cpp
cam.save_next();     // any camera
cam.save_next(0);    // camera 0 only

// Change the save directory at runtime:
cam.set_save_dir("C:\\CameraImages\\Session1");
```

---

## Multi-Camera

Run one `GigECamera` object per thread, or share a single object and call `grab()` on different `camera_id` values from multiple threads. `GigECamera` is not thread-safe — use a mutex if sharing.

### Pattern A — shared object, two grabs per frame tick

```cpp
GigECamera cam("localhost:50051");
cam.start();   // starts all cameras

int64_t ts0 = 0, ts1 = 0;
while (running) {
    auto f0 = cam.grab_wait(0, ts0, 100);
    auto f1 = cam.grab_wait(1, ts1, 100);

    if (f0) { ts0 = f0->timestamp_ms; /* process f0 */ }
    if (f1) { ts1 = f1->timestamp_ms; /* process f1 */ }
}

cam.stop();
```

### Pattern B — one object per thread (fully independent)

```cpp
auto worker = [](const std::string& addr, int cam_id) {
    GigECamera cam(addr);
    cam.start(cam_id);

    int64_t last_ts = 0;
    while (running) {
        auto frame = cam.grab_wait(cam_id, last_ts, 500);
        if (!frame) continue;
        last_ts = frame->timestamp_ms;
        // ... process ...
    }
    cam.stop(cam_id);
};

std::thread t0(worker, "localhost:50051", 0);
std::thread t1(worker, "localhost:50051", 1);
t0.join();
t1.join();
```

---

## API Reference

### `GigeFrame`

Returned by `grab()` / `grab_wait()`. All data is owned — safe to store or move.

| Field | Type | Description |
|---|---|---|
| `pixels` | `std::vector<uint8_t>` | RGB8 interleaved pixel data |
| `width` | `int32_t` | Frame width in pixels |
| `height` | `int32_t` | Frame height in pixels |
| `channels` | `int32_t` | Always `3` (RGB) |
| `camera_id` | `int32_t` | 0-based index of the source camera |
| `timestamp_ms` | `int64_t` | Server-side capture timestamp (ms since epoch) |

### `GigeCameraInfo`

Returned by `cam.info(camera_id)`.

| Field | Type | Description |
|---|---|---|
| `camera_id` | `int32_t` | 0-based index |
| `model_name` | `std::string` | Camera model string |
| `serial` | `std::string` | Serial number |
| `ip_address` | `std::string` | GigE IP address |
| `width` / `height` | `int32_t` | Current ROI dimensions |
| `offset_x` / `offset_y` | `int32_t` | Current ROI offsets |
| `binning_h` / `binning_v` | `int32_t` | Current binning factors |
| `exposure_us` | `float` | Exposure time in microseconds |
| `gain_db` | `float` | Gain in dB |
| `gamma` | `float` | Gamma correction value |
| `black_level` | `float` | Black level offset |
| `frame_rate` | `float` | Configured frame rate |
| `fps` | `float` | Measured actual frame rate |
| `acquiring` | `bool` | Whether acquisition is active |
| `exposure_auto` | `std::string` | `"Off"` / `"Once"` / `"Continuous"` |
| `gain_auto` | `std::string` | `"Off"` / `"Once"` / `"Continuous"` |

### `GigeSystemState`

Returned by `cam.state()`.

| Field | Type | Description |
|---|---|---|
| `status` | `std::string` | `"IDLE"` \| `"ACQUIRING"` \| `"PARTIAL"` \| `"ERROR"` |
| `connected_cameras` | `int32_t` | Number of detected cameras |
| `current_fps` | `float` | Aggregate frame rate |

### `GigECamera` methods

| Method | Returns | Description |
|---|---|---|
| `GigECamera(address)` | — | Connect to server (non-blocking) |
| `state()` | `GigeSystemState` | Module health snapshot |
| `info(camera_id=0)` | `optional<GigeCameraInfo>` | Full camera state |
| `start(camera_id=-1)` | `bool` | Start acquisition (-1 = all) |
| `stop(camera_id=-1)` | `bool` | Stop acquisition (-1 = all) |
| `grab(camera_id=0)` | `optional<GigeFrame>` | Latest frame, non-blocking |
| `grab_any()` | `optional<GigeFrame>` | Latest frame from any camera |
| `grab_wait(cam, last_ts, timeout_ms)` | `optional<GigeFrame>` | Block until new frame or timeout |
| `grab_any_wait(last_ts, timeout_ms)` | `optional<GigeFrame>` | Block until new frame from any camera |
| `set_exposure(us, camera_id=-1)` | `bool` | Exposure in microseconds (requires `ExposureAuto="Off"`) |
| `set_gain(db, camera_id=-1)` | `bool` | Gain in dB (requires `GainAuto="Off"`) |
| `set_gamma(gamma, camera_id=-1)` | `bool` | Gamma correction value |
| `set_frame_rate(fps, camera_id=-1)` | `bool` | Acquisition frame rate |
| `set_roi(w, h, ox, oy, camera_id=-1)` | `bool` | Region of interest |
| `set_exposure_auto(mode, camera_id=-1)` | `bool` | `"Off"` / `"Once"` / `"Continuous"` |
| `set_gain_auto(mode, camera_id=-1)` | `bool` | `"Off"` / `"Once"` / `"Continuous"` |
| `set_param(name, float_val, int_val, camera_id=-1, string_val="")` | `bool` | Any GenICam node by name |
| `save_next(camera_id=-1)` | `void` | Queue next frame for JPEG disk save |
| `set_save_dir(path)` | `bool` | Change the server's save directory |

---

## Examples

| File | What it shows |
|---|---|
| [`simple_grab.cpp`](simple_grab.cpp) | Connect → start → grab 10 frames → stop |
| [`camera_control.cpp`](camera_control.cpp) | Inspect camera, adjust exposure/gain/ROI, save to disk |
| [`CMakeLists.txt`](CMakeLists.txt) | CMake setup for both examples |
