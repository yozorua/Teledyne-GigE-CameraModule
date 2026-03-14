"""
camera_client.py
~~~~~~~~~~~~~~~~
Example consumer for GigECameraModule.

Demonstrates:
  - Connecting to the gRPC server
  - Querying system and camera state
  - Setting camera parameters
  - Grabbing frames via GetLatestImageFrame / ReleaseImageFrame
  - Reading raw pixel data directly from Windows Shared Memory
  - Converting pixels to a numpy array / PIL Image

Prerequisites
-------------
1. Install dependencies:
       pip install -r requirements.txt

2. Generate gRPC stubs once:
       generate_proto.bat

3. GigECameraModule.exe must be running (as Administrator).

Usage
-----
    python camera_client.py [address] [camera_id] [frame_count]
    python camera_client.py localhost:50051 0 100
"""

from __future__ import annotations

import ctypes
import sys
import time
from dataclasses import dataclass
from typing import Optional

import grpc
import numpy as np
from PIL import Image

import camera_service_pb2 as pb
import camera_service_pb2_grpc as pb_grpc

# ─────────────────────────────────────────────────────────────────────────────
# Shared Memory constants and layout
# Must match include/SharedMemoryManager.h exactly.
# ─────────────────────────────────────────────────────────────────────────────

SHM_NAME     = "Global\\CameraImageBufferPool"
POOL_SIZE    = 20
MAX_CAMERAS  = 4
SHM_WRITING  = -1   # sentinel: producer is writing into this slot


class SharedMemoryHeader(ctypes.Structure):
    """
    Mirrors the C++ SharedMemoryHeader struct.
    std::atomic<int32_t> on MSVC/x64 is lock-free and has the same size and
    alignment as a plain int32, so it maps directly to c_int32 here.

    Layout (verified: ctypes.sizeof == 368 bytes):
      offset  0  : latest_buffer_index          c_int32        4
      offset  4  : latest_buffer_per_camera[4]  c_int32 * 4   16
      offset 20  : image_width                  c_int32        4
      offset 24  : image_height                 c_int32        4
      offset 28  : image_channels               c_int32        4
      offset 32  : single_image_size            c_size_t       8  (8-byte aligned)
      offset 40  : pool_size                    c_int32        4
      offset 44  : num_cameras                  c_int32        4
      offset 48  : buffer_camera_id[20]         c_int32 * 20  80
      offset 128 : buffer_width[20]             c_int32 * 20  80
      offset 208 : buffer_height[20]            c_int32 * 20  80
      offset 288 : reference_counts[20]         c_int32 * 20  80
      total: 368
    """
    _fields_ = [
        ("latest_buffer_index",      ctypes.c_int32),
        ("latest_buffer_per_camera", ctypes.c_int32 * MAX_CAMERAS),
        ("image_width",              ctypes.c_int32),
        ("image_height",             ctypes.c_int32),
        ("image_channels",           ctypes.c_int32),
        ("single_image_size",        ctypes.c_size_t),   # 8 bytes on x64
        ("pool_size",                ctypes.c_int32),
        ("num_cameras",              ctypes.c_int32),
        ("buffer_camera_id",         ctypes.c_int32 * POOL_SIZE),
        ("buffer_width",             ctypes.c_int32 * POOL_SIZE),
        ("buffer_height",            ctypes.c_int32 * POOL_SIZE),
        ("reference_counts",         ctypes.c_int32 * POOL_SIZE),
    ]


# Validate struct size at import time — catches layout mismatches immediately.
_expected_header_size = 368
assert ctypes.sizeof(SharedMemoryHeader) == _expected_header_size, (
    f"SharedMemoryHeader size mismatch: "
    f"got {ctypes.sizeof(SharedMemoryHeader)}, expected {_expected_header_size}. "
    "Check that POOL_SIZE and MAX_CAMERAS match the C++ header."
)

# Image data starts immediately after the header.
IMAGE_DATA_OFFSET = ctypes.sizeof(SharedMemoryHeader)   # 368


# ─────────────────────────────────────────────────────────────────────────────
# ShmReader — opens the shared memory block read-only
# ─────────────────────────────────────────────────────────────────────────────

FILE_MAP_READ       = 0x0004
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

_kernel32 = ctypes.windll.kernel32


class ShmReader:
    """
    Opens Global\\CameraImageBufferPool read-only.

    No Administrator privileges needed for read-only access.
    """

    def __init__(self) -> None:
        self._mapping: Optional[int] = None
        self._view:    Optional[ctypes.c_void_p] = None
        self._base:    Optional[int] = None
        self.header:   Optional[SharedMemoryHeader] = None
        self._total_size = 0

    def open(self) -> bool:
        mapping = _kernel32.OpenFileMappingA(
            FILE_MAP_READ,
            False,
            SHM_NAME.encode()
        )
        if not mapping:
            err = ctypes.get_last_error()
            print(f"[ShmReader] OpenFileMappingA failed: error {err}", file=sys.stderr)
            return False

        view = _kernel32.MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)
        if not view:
            err = ctypes.get_last_error()
            print(f"[ShmReader] MapViewOfFile failed: error {err}", file=sys.stderr)
            _kernel32.CloseHandle(mapping)
            return False

        self._mapping = mapping
        self._base    = view

        # Overlay the header struct onto the mapped memory.
        self.header = SharedMemoryHeader.from_address(view)

        print(
            f"[ShmReader] Opened SHM  "
            f"{self.header.image_width}x{self.header.image_height}"
            f"x{self.header.image_channels}"
            f"  pool={self.header.pool_size}"
            f"  cameras={self.header.num_cameras}"
        )
        return True

    def is_open(self) -> bool:
        return self._base is not None

    def buffer_as_ndarray(self, idx: int) -> np.ndarray:
        """
        Returns a *view* (zero-copy) into buffer slot *idx* as a numpy array.

        The returned array is valid only while the buffer is pinned
        (i.e. between GetLatestImageFrame and ReleaseImageFrame).
        If you need the data to outlive the release, call .copy() on it.

        The array shape is (height, width) for single-channel (Mono8) images.
        """
        if self.header is None:
            raise RuntimeError("ShmReader not open")
        if not (0 <= idx < POOL_SIZE):
            raise IndexError(f"Buffer index {idx} out of range [0, {POOL_SIZE})")

        w    = self.header.buffer_width[idx]
        h    = self.header.buffer_height[idx]
        size = self.header.single_image_size
        addr = self._base + IMAGE_DATA_OFFSET + idx * size   # type: ignore[operator]

        # Create a numpy array backed by the SHM bytes (no copy).
        raw = (ctypes.c_uint8 * (w * h)).from_address(addr)
        arr = np.frombuffer(raw, dtype=np.uint8).reshape(h, w)
        return arr

    def close(self) -> None:
        if self._base is not None:
            _kernel32.UnmapViewOfFile(self._base)
            self._base = None
        if self._mapping is not None:
            _kernel32.CloseHandle(self._mapping)
            self._mapping = None
        self.header = None

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *_):
        self.close()


# ─────────────────────────────────────────────────────────────────────────────
# GrpcClient — thin wrapper around the generated stub
# ─────────────────────────────────────────────────────────────────────────────

class GrpcClient:
    def __init__(self, address: str = "localhost:50051") -> None:
        self._channel = grpc.insecure_channel(address)
        self._stub    = pb_grpc.CameraControlStub(self._channel)

    # -- System state ----------------------------------------------------------

    def get_system_state(self) -> pb.SystemState:
        return self._stub.GetSystemState(pb.Empty())

    # -- Camera info -----------------------------------------------------------

    def get_camera_info(self, camera_id: int) -> pb.CameraState:
        return self._stub.GetCameraInfo(pb.CameraRequest(camera_id=camera_id))

    # -- Acquisition control ---------------------------------------------------

    def start_acquisition(self, camera_id: int = -1) -> pb.CommandStatus:
        return self._stub.StartAcquisition(pb.CameraRequest(camera_id=camera_id))

    def stop_acquisition(self, camera_id: int = -1) -> pb.CommandStatus:
        return self._stub.StopAcquisition(pb.CameraRequest(camera_id=camera_id))

    # -- Parameter control -----------------------------------------------------

    def set_float_param(self, name: str, value: float, camera_id: int = -1) -> pb.CommandStatus:
        """Set a float parameter (ExposureTime, Gain, Gamma, AcquisitionFrameRate, ...)."""
        return self._stub.SetParameter(pb.ParameterRequest(
            camera_id=camera_id, param_name=name, float_value=value
        ))

    def set_int_param(self, name: str, value: int, camera_id: int = -1) -> pb.CommandStatus:
        """Set an integer parameter (Width, Height, OffsetX, OffsetY, Binning*, ...)."""
        return self._stub.SetParameter(pb.ParameterRequest(
            camera_id=camera_id, param_name=name, int_value=value
        ))

    # -- Frame acquisition -----------------------------------------------------

    def get_latest_frame(self, camera_id: int = 0) -> pb.FrameInfo:
        """
        Pins the latest buffer for *camera_id* and returns metadata.

        **You must call release_frame() with FrameInfo.shared_memory_index
        when you are done with the pixel data.**
        """
        return self._stub.GetLatestImageFrame(pb.FrameRequest(camera_id=camera_id))

    def release_frame(self, shm_index: int) -> pb.CommandStatus:
        return self._stub.ReleaseImageFrame(pb.ReleaseRequest(shared_memory_index=shm_index))

    # -- Disk save -------------------------------------------------------------

    def trigger_disk_save(self, camera_id: int = -1) -> pb.CommandStatus:
        return self._stub.TriggerDiskSave(pb.CameraRequest(camera_id=camera_id))

    def set_save_directory(self, path: str) -> pb.CommandStatus:
        return self._stub.SetSaveDirectory(pb.SaveDirectoryRequest(path=path))

    def close(self) -> None:
        self._channel.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


# ─────────────────────────────────────────────────────────────────────────────
# Demo: grab N frames, print stats, optionally save last frame as PNG
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class FrameStats:
    frame_num:    int
    camera_id:    int
    width:        int
    height:       int
    min_val:      int
    max_val:      int
    mean_val:     float
    timestamp_ms: int


def main() -> None:
    address     = sys.argv[1] if len(sys.argv) > 1 else "localhost:50051"
    camera_id   = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    frame_count = int(sys.argv[3]) if len(sys.argv) > 3 else 50

    print("GigE Camera Analyzer — Python Example")
    print(f"  server  : {address}")
    print(f"  camera  : {camera_id}")
    print(f"  frames  : {frame_count}\n")

    with GrpcClient(address) as client:
        # -- 1. Check server health ----------------------------------------------
        state = client.get_system_state()
        print(f"System state : {state.status}")
        print(f"Cameras      : {state.connected_cameras}\n")

        # -- 2. Print camera info ------------------------------------------------
        try:
            info = client.get_camera_info(camera_id)
            print(f"Camera {camera_id} info:")
            print(f"  Model      : {info.model_name}")
            print(f"  Serial     : {info.serial}")
            print(f"  IP         : {info.ip_address}")
            print(f"  ROI        : {info.width}x{info.height}  @({info.offset_x},{info.offset_y})")
            print(f"  Binning    : {info.binning_h}x{info.binning_v}")
            print(f"  Exposure   : {info.exposure_us:.1f} us")
            print(f"  Gain       : {info.gain_db:.2f} dB")
            print(f"  Gamma      : {info.gamma:.2f}")
            print(f"  Frame rate : {info.frame_rate:.1f} fps")
            print(f"  Acquiring  : {info.acquiring}\n")
        except grpc.RpcError as e:
            print(f"  (GetCameraInfo failed: {e.code()})\n")

        # -- 3. (Optional) configure camera before grabbing ----------------------
        # Uncomment to adjust settings, e.g.:
        #   client.set_float_param("ExposureTime", 10_000.0, camera_id)  # 10 ms
        #   client.set_float_param("Gain",          5.0,     camera_id)
        #   client.set_int_param("Width",           1280,    camera_id)
        #   client.set_int_param("Height",          1024,    camera_id)
        #   client.set_int_param("OffsetX",          320,    camera_id)
        #   client.set_int_param("OffsetY",          240,    camera_id)

        # -- 4. Start acquisition if needed --------------------------------------
        was_idle = state.status in ("IDLE", "ERROR")
        if was_idle:
            print(f"Starting acquisition on camera {camera_id}...")
            r = client.start_acquisition(camera_id)
            if not r.success:
                print(f"ERROR: {r.message}", file=sys.stderr)
                return
            time.sleep(0.2)  # let first frames arrive

        # -- 5. Open shared memory (read-only, no admin required) ----------------
        shm = ShmReader()
        if not shm.open():
            print("ERROR: Could not open shared memory.", file=sys.stderr)
            if was_idle:
                client.stop_acquisition(camera_id)
            return

        # -- 6. Frame grab loop --------------------------------------------------
        print(f"\nGrabbing {frame_count} frames...\n")
        print(f"  {'#':>5}  {'cam':>3}  {'WxH':<14}  {'min':>3}  {'max':>3}  {'mean':>8}")
        print(f"  {'-----':>5}  {'---':>3}  {'----------':<14}  {'---':>3}  {'---':>3}  {'--------':>8}")

        grabbed   = 0
        failures  = 0
        last_arr: Optional[np.ndarray] = None

        for i in range(frame_count):
            try:
                frame = client.get_latest_frame(camera_id)
            except grpc.RpcError as e:
                failures += 1
                time.sleep(0.01)
                continue

            idx = frame.shared_memory_index
            w   = frame.width
            h   = frame.height

            # Access pixels directly from shared memory (zero-copy view).
            arr = shm.buffer_as_ndarray(idx)

            # Compute stats while the buffer is pinned.
            mn  = int(arr.min())
            mx  = int(arr.max())
            avg = float(arr.mean())

            # If you need the data to outlive the release, copy it:
            if i == frame_count - 1:
                last_arr = arr.copy()   # own copy of the last frame

            # Release as soon as you are done reading to free the slot.
            client.release_frame(idx)

            grabbed += 1
            print(f"  {grabbed:>5}  {frame.camera_id:>3}  {w:>5}x{h:<8}  {mn:>3}  {mx:>3}  {avg:>8.2f}")

            # ── Your processing goes here ─────────────────────────────────────
            #
            # arr  is a numpy array of shape (h, w) dtype=uint8 (Mono8).
            # It is a *live view* into SHM — valid only before release_frame().
            #
            # For heavier work, copy first:
            #   owned = arr.copy()
            #   client.release_frame(idx)
            #   ... process owned ...
            #
            # Convert to PIL for display / saving:
            #   img = Image.fromarray(arr, mode="L")
            #   img.save("frame.png")
            #
            # ─────────────────────────────────────────────────────────────────

        shm.close()

        print(f"\nDone. Grabbed {grabbed} frames, {failures} failures.")

        # Save the last captured frame as a PNG for inspection.
        if last_arr is not None:
            out_path = f"last_frame_cam{camera_id}.png"
            img = Image.fromarray(last_arr, mode="L")
            img.save(out_path)
            print(f"Last frame saved to: {out_path}")

        # -- 7. Stop acquisition if we started it --------------------------------
        if was_idle:
            print(f"Stopping acquisition on camera {camera_id}...")
            client.stop_acquisition(camera_id)


if __name__ == "__main__":
    main()
