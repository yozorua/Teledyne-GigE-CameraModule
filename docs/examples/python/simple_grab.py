"""
simple_grab.py
~~~~~~~~~~~~~~
Minimal example: connect, start, grab 10 frames, stop.

Usage:
    python simple_grab.py [address] [camera_id]
    python simple_grab.py localhost:50051 0

Requires:
    pip install -r requirements.txt
    generate_proto.bat   (run once)
"""

import sys
from gige_camera import GigECamera, GrabError
from PIL import Image

address   = sys.argv[1] if len(sys.argv) > 1 else "localhost:50051"
camera_id = int(sys.argv[2]) if len(sys.argv) > 2 else 0

with GigECamera(address) as cam:
    # ── Check server is reachable ─────────────────────────────────────────────
    state = cam.state()
    print(f"Server: {address}  status={state.status}  cameras={state.connected_cameras}")

    # ── (Optional) print camera details ──────────────────────────────────────
    info = cam.info(camera_id)
    print(f"Camera {camera_id}: {info.model_name}  {info.width}x{info.height}"
          f"  exp={info.exposure_us:.0f}us  gain={info.gain_db:.1f}dB")

    # ── (Optional) adjust settings before grabbing ────────────────────────────
    # cam.set_exposure(5_000.0, camera_id)   # 5 ms
    # cam.set_gain(3.0, camera_id)
    # cam.set_roi(1280, 720, offset_x=320, offset_y=180, camera_id=camera_id)

    # ── Start acquisition ─────────────────────────────────────────────────────
    cam.start(camera_id)

    # ── Grab frames ───────────────────────────────────────────────────────────
    print("\nGrabbing 10 frames...\n")
    for i in range(10):
        try:
            frame = cam.grab(camera_id)
        except GrabError as e:
            print(f"  [{i}] {e}")
            continue

        mean = frame.image.mean()
        print(f"  [{i}] cam={frame.camera_id}  {frame.width}x{frame.height}"
              f"  mean={mean:.1f}  ts={frame.timestamp_ms}")

    # ── Save last frame as PNG ────────────────────────────────────────────────
    img = Image.fromarray(frame.image, mode="RGB")
    img.save(f"last_frame_cam{camera_id}.png")
    print(f"\nSaved last frame to last_frame_cam{camera_id}.png")

    # ── Stop acquisition ──────────────────────────────────────────────────────
    cam.stop(camera_id)
