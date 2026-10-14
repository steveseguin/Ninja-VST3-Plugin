"""Capture the real Windows VST3 editor, without recording the desktop or audio.

Requires a built webrtc_vst_windows_editor_test and FFmpeg on PATH (or --ffmpeg).
Run from any directory; output defaults to media/gallery in this repository.
"""
import argparse
import ctypes as ct
from ctypes import wintypes as wt
from pathlib import Path
import subprocess
import sys
import time


def main():
    if sys.platform != "win32":
        raise SystemExit("This capture tool requires Windows.")
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--encoder", default="libx264", choices=("libx264", "libopenh264"))
    parser.add_argument("--build-dir", type=Path, default=root / "build/webrtc_vst_win")
    parser.add_argument("--output", type=Path, default=root / "media/gallery")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    logs = root / "build/gallery-capture"
    logs.mkdir(parents=True, exist_ok=True)
    user, gdi = ct.windll.user32, ct.windll.gdi32
    user.SetProcessDpiAwarenessContext.argtypes = [ct.c_void_p]
    user.SetProcessDpiAwarenessContext(ct.c_void_p(-4))
    user.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
    user.FindWindowW.restype = wt.HWND
    user.GetWindowThreadProcessId.argtypes = [wt.HWND, ct.POINTER(wt.DWORD)]
    user.GetClientRect.argtypes = [wt.HWND, ct.POINTER(wt.RECT)]
    user.GetDC.argtypes = [wt.HWND]
    user.GetDC.restype = wt.HDC
    user.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
    user.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
    gdi.CreateCompatibleDC.argtypes = [wt.HDC]
    gdi.CreateCompatibleDC.restype = wt.HDC
    gdi.SelectObject.argtypes = [wt.HDC, wt.HANDLE]
    gdi.SelectObject.restype = wt.HANDLE
    gdi.DeleteObject.argtypes = [wt.HANDLE]
    gdi.DeleteDC.argtypes = [wt.HDC]

    class BitmapInfo(ct.Structure):
        _fields_ = [("size", wt.DWORD), ("width", wt.LONG), ("height", wt.LONG),
                    ("planes", wt.WORD), ("bits", wt.WORD), ("compression", wt.DWORD),
                    ("image_size", wt.DWORD), ("xppm", wt.LONG), ("yppm", wt.LONG),
                    ("colors", wt.DWORD), ("important", wt.DWORD)]

    gdi.CreateDIBSection.argtypes = [wt.HDC, ct.POINTER(BitmapInfo), wt.UINT,
                                   ct.POINTER(ct.c_void_p), wt.HANDLE, wt.DWORD]
    gdi.CreateDIBSection.restype = wt.HBITMAP
    width, height, fps = 720, 340, 12
    with (logs / "host.log").open("w") as host_log, (logs / "ffmpeg.log").open("w") as encoder_log:
        host = subprocess.Popen([
            str(args.build_dir / "bin/Release/webrtc_vst_windows_editor_test.exe"),
            str(args.build_dir / "VST3/Release/webrtc_vst.vst3"), str(args.output.resolve())],
            stdout=host_log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        encoder = None
        window = dc = memory = bitmap = old = None
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and host.poll() is None:
                window = user.FindWindowW("WebRTCEditorSmokeHost", None)
                pid = wt.DWORD()
                if window:
                    user.GetWindowThreadProcessId(window, ct.byref(pid))
                    if pid.value == host.pid:
                        break
                time.sleep(0.05)
            else:
                raise RuntimeError("Owned editor window did not appear; see build/gallery-capture/host.log")
            time.sleep(0.3)
            dc = user.GetDC(window)
            memory = gdi.CreateCompatibleDC(dc)
            info = BitmapInfo(ct.sizeof(BitmapInfo), width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
            pixels = ct.c_void_p()
            bitmap = gdi.CreateDIBSection(dc, ct.byref(info), 0, ct.byref(pixels), None, 0)
            if not bitmap or not pixels.value:
                raise RuntimeError("Cannot allocate capture bitmap")
            old = gdi.SelectObject(memory, bitmap)
            encoder = subprocess.Popen([
                args.ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "bgr0", "-video_size", "720x340",
                "-framerate", str(fps), "-i", "pipe:0", "-an", "-c:v", args.encoder, "-b:v", "2M",
                "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(args.output / "editor-demo.mp4")],
                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=encoder_log,
                creationflags=subprocess.CREATE_NO_WINDOW)
            started = time.monotonic()
            frames = 0
            for index in range(8 * fps):
                rect = wt.RECT()
                if not user.GetClientRect(window, ct.byref(rect)) or (rect.right, rect.bottom) != (width, height):
                    break  # End before the smoke test starts its DPI-resize checks.
                if not user.PrintWindow(window, memory, 3):
                    raise RuntimeError("Editor-only capture failed")
                gdi.GdiFlush()
                encoder.stdin.write(ct.string_at(pixels, width * height * 4))
                frames += 1
                time.sleep(max(0, started + (index + 1) / fps - time.monotonic()))
            encoder.stdin.close()
            if encoder.wait(timeout=15) or frames < fps:
                raise RuntimeError("Incomplete video; see build/gallery-capture/ffmpeg.log")
            if host.wait(timeout=60):
                raise RuntimeError("Editor validation failed; see build/gallery-capture/host.log")
            print(f"Captured {frames / fps:.2f}s of the native editor and three PNGs in {args.output}")
        finally:
            if encoder and encoder.poll() is None:
                encoder.kill()
                encoder.wait()
            if host.poll() is None:
                host.kill()
                host.wait()
            if memory and old:
                gdi.SelectObject(memory, old)
            if bitmap:
                gdi.DeleteObject(bitmap)
            if memory:
                gdi.DeleteDC(memory)
            if dc and window:
                user.ReleaseDC(window, dc)


if __name__ == "__main__":
    main()
