import argparse
import ctypes
import struct
import zlib

import win32con
import win32gui
import win32process
import win32ui


def png_chunk(kind, payload):
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def find_window(pid):
    matches = []

    def visit(hwnd, _):
        title = win32gui.GetWindowText(hwnd)
        if (
            not win32gui.IsWindowVisible(hwnd)
            or win32gui.GetClassName(hwnd) != "JammaWindow"
            or not (title == "Jamma" or title.startswith("Jamma - "))
        ):
            return
        _, window_pid = win32process.GetWindowThreadProcessId(hwnd)
        if window_pid == pid:
            matches.append(hwnd)

    win32gui.EnumWindows(visit, None)
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one visible Jamma window for PID {pid}, found {len(matches)}"
        )
    return matches[0]


def capture(pid, output):
    hwnd = find_window(pid)
    left, top, right, bottom = win32gui.GetWindowRect(hwnd)
    width, height = right - left, bottom - top
    if width <= 0 or height <= 0:
        raise RuntimeError(f"invalid window bounds: {(left, top, right, bottom)}")

    window_dc = win32gui.GetWindowDC(hwnd)
    source_dc = win32ui.CreateDCFromHandle(window_dc)
    memory_dc = source_dc.CreateCompatibleDC()
    bitmap = win32ui.CreateBitmap()
    bitmap.CreateCompatibleBitmap(source_dc, width, height)
    memory_dc.SelectObject(bitmap)
    try:
        rendered = ctypes.windll.user32.PrintWindow(hwnd, memory_dc.GetSafeHdc(), 2)
        if rendered != 1:
            memory_dc.BitBlt(
                (0, 0), (width, height), source_dc, (0, 0), win32con.SRCCOPY
            )
        bits = bitmap.GetBitmapBits(True)
    finally:
        try:
            win32gui.ReleaseDC(hwnd, window_dc)
        except Exception:
            pass
        for device_context in (source_dc, memory_dc):
            try:
                device_context.DeleteDC()
            except Exception:
                pass
        try:
            win32gui.DeleteObject(bitmap.GetHandle())
        except Exception:
            pass

    stride = width * 4
    scanlines = bytearray()
    for y in range(height):
        row = bits[y * stride : (y + 1) * stride]
        rgba = bytearray()
        for x in range(0, len(row), 4):
            blue, green, red, _ = row[x : x + 4]
            rgba.extend((red, green, blue, 255))
        scanlines.append(0)
        scanlines.extend(rgba)

    png = b"\x89PNG\r\n\x1a\n"
    png += png_chunk(
        b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    )
    png += png_chunk(b"IDAT", zlib.compress(bytes(scanlines), 6))
    png += png_chunk(b"IEND", b"")
    with open(output, "wb") as handle:
        handle.write(png)
    print(
        {
            "pid": pid,
            "hwnd": hwnd,
            "bounds": [left, top, right, bottom],
            "print_window": rendered,
            "path": output,
            "capture_method": "win32-printwindow-fallback",
        }
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Capture one validated Jamma window")
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    capture(args.pid, args.output)
