"""
gige_camera.py
~~~~~~~~~~~~~~
Single-file Python wrapper for GigECameraModule.

Hides all gRPC boilerplate and shared-memory layout details.
Your application only needs this file + the generated proto stubs.

Requirements:
    pip install -r requirements.txt
    generate_proto.bat          # run once to generate proto stubs

Quick start:
    from gige_camera import GigECamera

    with GigECamera("localhost:50051") as cam:
        cam.start()
        frame = cam.grab(camera_id=0)
        # frame.image is a numpy array  shape=(h, w, 3)  dtype=uint8  RGB
        print(frame.image.shape, frame.image.mean())
        cam.stop()
"""

from __future__ import annotations

import ctypes
import sys
import time
from dataclasses import dataclass, field
from typing import Optional

import grpc
import numpy as np

# ── Proto stubs ───────────────────────────────────────────────────────────────
try:
    import camera_service_pb2 as _pb
    import camera_service_pb2_grpc as _pb_grpc
except ImportError:
    raise ImportError(
        "gRPC stubs not found.\n"
        "Run  generate_proto.bat  once to generate them, then retry."
    )

# ─────────────────────────────────────────────────────────────────────────────
# Public data types
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class Frame:
    """A single camera frame returned by GigECamera.grab()."""
    image:        np.ndarray   # shape (height, width, 3), dtype uint8, RGB8
    camera_id:    int
    timestamp_ms: int
    width:        int
    height:       int


@dataclass
class CameraInfo:
    """Live state snapshot for one camera."""
    camera_id:   int
    model_name:  str
    serial:      str
    ip_address:  str
    width:       int
    height:      int
    offset_x:    int
    offset_y:    int
    binning_h:   int
    binning_v:   int
    exposure_us: float
    gain_db:     float
    gamma:       float
    black_level: float
    frame_rate:  float
    fps:         float
    acquiring:   bool


@dataclass
class SystemState:
    """Module-level health snapshot."""
    status:           str    # "IDLE" | "ACQUIRING" | "PARTIAL" | "ERROR"
    connected_cameras: int
    current_fps:      float


class GrabError(RuntimeError):
    """Raised when no frame is available from the requested camera."""


# ─────────────────────────────────────────────────────────────────────────────
# Internal: shared-memory reader
# Mirrors include/SharedMemoryManager.h exactly.
# ─────────────────────────────────────────────────────────────────────────────

_SHM_NAME       = "Global\\CameraImageBufferPool"
_POOL_SIZE      = 20
_MAX_CAMERAS    = 4
_FILE_MAP_READ  = 0x0004
_k32            = ctypes.windll.kernel32


class _ShmHeader(ctypes.Structure):
    _fields_ = [
        ("latest_buffer_index",      ctypes.c_int32),
        ("latest_buffer_per_camera", ctypes.c_int32 * _MAX_CAMERAS),
        ("image_width",              ctypes.c_int32),
        ("image_height",             ctypes.c_int32),
        ("image_channels",           ctypes.c_int32),
        ("single_image_size",        ctypes.c_size_t),
        ("pool_size",                ctypes.c_int32),
        ("num_cameras",              ctypes.c_int32),
        ("buffer_camera_id",         ctypes.c_int32 * _POOL_SIZE),
        ("buffer_width",             ctypes.c_int32 * _POOL_SIZE),
        ("buffer_height",            ctypes.c_int32 * _POOL_SIZE),
        ("reference_counts",         ctypes.c_int32 * _POOL_SIZE),
    ]

_IMAGE_DATA_OFFSET = ctypes.sizeof(_ShmHeader)   # 368 bytes

assert _IMAGE_DATA_OFFSET == 368, (
    f"SHM header size mismatch: {_IMAGE_DATA_OFFSET} != 368. "
    "Check that _POOL_SIZE and _MAX_CAMERAS match the C++ header."
)


class _ShmReader:
    """Opens Global\\CameraImageBufferPool read-only (no admin required)."""

    def __init__(self) -> None:
        self._mapping: Optional[int] = None
        self._base:    Optional[int] = None
        self.header:   Optional[_ShmHeader] = None

    def open(self) -> bool:
        mapping = _k32.OpenFileMappingA(_FILE_MAP_READ, False, _SHM_NAME.encode())
        if not mapping:
            return False
        view = _k32.MapViewOfFile(mapping, _FILE_MAP_READ, 0, 0, 0)
        if not view:
            _k32.CloseHandle(mapping)
            return False
        self._mapping = mapping
        self._base    = view
        self.header   = _ShmHeader.from_address(view)
        return True

    def is_open(self) -> bool:
        return self._base is not None

    def read_frame(self, idx: int) -> np.ndarray:
        """Returns a *copy* of the pixel data for buffer slot idx (RGB8)."""
        assert self.header is not None
        w    = self.header.buffer_width[idx]
        h    = self.header.buffer_height[idx]
        ch   = self.header.image_channels
        size = self.header.single_image_size
        addr = self._base + _IMAGE_DATA_OFFSET + idx * size  # type: ignore[operator]
        raw  = (ctypes.c_uint8 * (w * h * ch)).from_address(addr)
        # Return an owned copy — safe to use after the frame is released.
        return np.frombuffer(raw, dtype=np.uint8).reshape(h, w, ch).copy()

    def close(self) -> None:
        if self._base    is not None: _k32.UnmapViewOfFile(self._base);    self._base    = None
        if self._mapping is not None: _k32.CloseHandle(self._mapping);     self._mapping = None
        self.header = None


# ─────────────────────────────────────────────────────────────────────────────
# Public API
# ─────────────────────────────────────────────────────────────────────────────

class GigECamera:
    """
    Simple interface to GigECameraModule.

    Combines gRPC control + shared-memory frame access into one object.
    Shared memory is opened lazily on the first grab() call.

    Parameters
    ----------
    address : str
        gRPC server address, e.g. "localhost:50051".
    """

    def __init__(self, address: str = "localhost:50051") -> None:
        self._address = address
        self._channel = grpc.insecure_channel(address)
        self._stub    = _pb_grpc.CameraControlStub(self._channel)
        self._shm     = _ShmReader()

    # ── Context manager ───────────────────────────────────────────────────────

    def __enter__(self) -> "GigECamera":
        return self

    def __exit__(self, *_) -> None:
        self.close()

    def close(self) -> None:
        """Release all resources (gRPC channel + SHM mapping)."""
        self._shm.close()
        self._channel.close()

    # ── System-level control ──────────────────────────────────────────────────

    def state(self) -> SystemState:
        """Return current module health (status, camera count, aggregate FPS)."""
        r = self._stub.GetSystemState(_pb.Empty())
        return SystemState(
            status=r.status,
            connected_cameras=r.connected_cameras,
            current_fps=r.current_fps,
        )

    def start(self, camera_id: int = -1) -> bool:
        """
        Start acquisition.

        Parameters
        ----------
        camera_id : int
            0-based camera index.  -1 (default) starts all cameras.
        """
        r = self._stub.StartAcquisition(_pb.CameraRequest(camera_id=camera_id))
        return r.success

    def stop(self, camera_id: int = -1) -> bool:
        """
        Stop acquisition.

        Parameters
        ----------
        camera_id : int
            0-based camera index.  -1 (default) stops all cameras.
        """
        r = self._stub.StopAcquisition(_pb.CameraRequest(camera_id=camera_id))
        return r.success

    # ── Camera info ───────────────────────────────────────────────────────────

    def info(self, camera_id: int = 0) -> CameraInfo:
        """Return full state snapshot for a single camera."""
        r = self._stub.GetCameraInfo(_pb.CameraRequest(camera_id=camera_id))
        return CameraInfo(
            camera_id=r.camera_id,
            model_name=r.model_name,
            serial=r.serial,
            ip_address=r.ip_address,
            width=r.width,
            height=r.height,
            offset_x=r.offset_x,
            offset_y=r.offset_y,
            binning_h=r.binning_h,
            binning_v=r.binning_v,
            exposure_us=r.exposure_us,
            gain_db=r.gain_db,
            gamma=r.gamma,
            black_level=r.black_level,
            frame_rate=r.frame_rate,
            fps=r.fps,
            acquiring=r.acquiring,
        )

    # ── Frame acquisition ─────────────────────────────────────────────────────

    def grab(self, camera_id: int = 0) -> Frame:
        """
        Grab the latest frame from a specific camera.

        The pixel data is copied out of shared memory before returning, so no
        explicit release is required — just use frame.image freely.

        Parameters
        ----------
        camera_id : int
            0-based camera index.

        Returns
        -------
        Frame
            frame.image is a numpy array  shape=(height, width, 3)  dtype=uint8
            in RGB byte order.

        Raises
        ------
        GrabError
            If no frame is available from the requested camera.
        """
        return self._grab_impl(camera_id)

    def grab_any(self) -> Frame:
        """
        Grab the latest frame from whichever camera produced it most recently.

        Raises
        ------
        GrabError
            If no frame is available from any camera.
        """
        return self._grab_impl(-1)

    def _grab_impl(self, camera_id: int) -> Frame:
        self._ensure_shm()
        try:
            meta = self._stub.GetLatestImageFrame(
                _pb.FrameRequest(camera_id=camera_id)
            )
        except grpc.RpcError as e:
            raise GrabError(
                f"No frame available from camera {camera_id}: {e.details()}"
            ) from e

        idx = meta.shared_memory_index
        try:
            image = self._shm.read_frame(idx)   # copies pixels out of SHM
        finally:
            # Always release, even if read_frame raises.
            self._stub.ReleaseImageFrame(_pb.ReleaseRequest(shared_memory_index=idx))

        return Frame(
            image=image,
            camera_id=meta.camera_id,
            timestamp_ms=meta.timestamp,
            width=meta.width,
            height=meta.height,
        )

    def _ensure_shm(self) -> None:
        if not self._shm.is_open():
            if not self._shm.open():
                raise RuntimeError(
                    f"Cannot open shared memory '{_SHM_NAME}'.\n"
                    "Ensure GigECameraModule.exe is running."
                )

    # ── Parameter control ─────────────────────────────────────────────────────

    def set_exposure(self, microseconds: float, camera_id: int = -1) -> bool:
        """Set exposure time in microseconds."""
        return self._set_float("ExposureTime", microseconds, camera_id)

    def set_gain(self, db: float, camera_id: int = -1) -> bool:
        """Set gain in dB."""
        return self._set_float("Gain", db, camera_id)

    def set_gamma(self, gamma: float, camera_id: int = -1) -> bool:
        """Set gamma correction value."""
        return self._set_float("Gamma", gamma, camera_id)

    def set_frame_rate(self, fps: float, camera_id: int = -1) -> bool:
        """Set acquisition frame rate."""
        return self._set_float("AcquisitionFrameRate", fps, camera_id)

    def set_roi(
        self,
        width: int,
        height: int,
        offset_x: int = 0,
        offset_y: int = 0,
        camera_id: int = -1,
    ) -> bool:
        """
        Set region of interest.

        Offsets are applied first on the camera, then width/height, to stay
        within sensor bounds.
        """
        ok  = self._set_int("OffsetX", 0,        camera_id)   # reset first
        ok &= self._set_int("OffsetY", 0,        camera_id)
        ok &= self._set_int("Width",   width,    camera_id)
        ok &= self._set_int("Height",  height,   camera_id)
        ok &= self._set_int("OffsetX", offset_x, camera_id)
        ok &= self._set_int("OffsetY", offset_y, camera_id)
        return ok

    def set_param(
        self,
        name: str,
        float_value: float = 0.0,
        int_value: int = 0,
        camera_id: int = -1,
    ) -> bool:
        """
        Set any GenICam node by name.

        Use float_value for analogue nodes (ExposureTime, Gain, Gamma, …).
        Use int_value for integer nodes (Width, Height, OffsetX, …).
        """
        r = self._stub.SetParameter(_pb.ParameterRequest(
            camera_id=camera_id,
            param_name=name,
            float_value=float_value,
            int_value=int_value,
        ))
        return r.success

    # ── Disk save ─────────────────────────────────────────────────────────────

    def save_next(self, camera_id: int = -1) -> None:
        """Queue the next frame from camera_id (or any camera) for JPEG disk save."""
        self._stub.TriggerDiskSave(_pb.CameraRequest(camera_id=camera_id))

    def set_save_dir(self, path: str) -> bool:
        """Change the directory where saved frames are written."""
        r = self._stub.SetSaveDirectory(_pb.SaveDirectoryRequest(path=path))
        return r.success

    # ── Helpers ───────────────────────────────────────────────────────────────

    def _set_float(self, name: str, value: float, camera_id: int) -> bool:
        r = self._stub.SetParameter(_pb.ParameterRequest(
            camera_id=camera_id, param_name=name, float_value=value
        ))
        return r.success

    def _set_int(self, name: str, value: int, camera_id: int) -> bool:
        r = self._stub.SetParameter(_pb.ParameterRequest(
            camera_id=camera_id, param_name=name, int_value=value
        ))
        return r.success
