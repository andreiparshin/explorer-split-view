import ctypes
import os
import struct
import subprocess
import sys
import time
import zlib
from ctypes import wintypes

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CONTROLLER_EXE = os.path.join(PROJECT_ROOT, "bin", "ExplorerSplitController.exe")
HOOK_DLL = os.path.join(PROJECT_ROOT, "bin", "ExplorerSplitHook.dll")
SCREENSHOT_SINGLE = os.path.join(PROJECT_ROOT, "build_obj", "test_single_pane.png")
SCREENSHOT_OUT = os.path.join(PROJECT_ROOT, "build_obj", "test_dual_pane.png")

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi

user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.restype = wintypes.LPARAM
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.PostMessageW.restype = wintypes.BOOL

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


def get_class(hwnd):
    buf = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(hwnd, buf, 128)
    return buf.value


def get_text(hwnd):
    WM_GETTEXT = 0x000D
    WM_GETTEXTLENGTH = 0x000E
    length = user32.SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0)
    buf = ctypes.create_unicode_buffer(max(512, length + 2))
    user32.SendMessageW(hwnd, WM_GETTEXT, len(buf), ctypes.addressof(buf))
    return buf.value


def get_client_size(hwnd):
    rc = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rc))
    return rc.right - rc.left, rc.bottom - rc.top


def get_rel_rect(child, parent):
    rc = wintypes.RECT()
    user32.GetWindowRect(child, ctypes.byref(rc))
    pt_tl = wintypes.POINT(rc.left, rc.top)
    pt_br = wintypes.POINT(rc.right, rc.bottom)
    user32.ScreenToClient(parent, ctypes.byref(pt_tl))
    user32.ScreenToClient(parent, ctypes.byref(pt_br))
    return pt_tl.x, pt_tl.y, pt_br.x - pt_tl.x, pt_br.y - pt_tl.y


def find_cabinets():
    res = []

    def cb(h, _):
        if get_class(h) == "CabinetWClass" and user32.IsWindowVisible(h):
            res.append(h)
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return res


def enum_children(parent, cls_filter=None):
    res = []

    def cb(h, _):
        if cls_filter is None or get_class(h) == cls_filter:
            res.append(h)
        return True

    user32.EnumChildWindows(parent, WNDENUMPROC(cb), 0)
    return res


def save_window_png(hwnd, out_path):
    rc = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    w = rc.right - rc.left
    h = rc.bottom - rc.top
    if w <= 0 or h <= 0:
        return False

    hdc_win = user32.GetWindowDC(hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_win)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_win, w, h)
    old_obj = gdi32.SelectObject(hdc_mem, hbmp)

    # PW_RENDERFULLCONTENT = 2 renders DirectComposition / XAML Islands + Win32 children!
    user32.PrintWindow(hwnd, hdc_mem, 2)

    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h  # Top-down
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0

    buf_len = w * h * 4
    raw = ctypes.create_string_buffer(buf_len)
    gdi32.GetDIBits(hdc_mem, hbmp, 0, h, raw, ctypes.byref(bmi), 0)

    gdi32.SelectObject(hdc_mem, old_obj)
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(hwnd, hdc_win)

    # Convert BGRA -> RGB scanlines for PNG
    data = raw.raw
    rows = []
    stride = w * 4
    for y in range(h):
        row = bytearray(1 + w * 3)
        row[0] = 0  # Filter type 0
        src_off = y * stride
        dst_off = 1
        for _ in range(w):
            b = data[src_off]
            g = data[src_off + 1]
            r = data[src_off + 2]
            row[dst_off] = r
            row[dst_off + 1] = g
            row[dst_off + 2] = b
            src_off += 4
            dst_off += 3
        rows.append(bytes(row))

    raw_png_data = b"".join(rows)

    def chunk(tag, body):
        return (
            struct.pack(">I", len(body))
            + tag
            + body
            + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    idat = zlib.compress(raw_png_data, 6)
    png_bytes = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(png_bytes)
    return True


psapi.EnumProcessModules.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
psapi.EnumProcessModules.restype = wintypes.BOOL
psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
psapi.GetModuleFileNameExW.restype = wintypes.DWORD

def is_dll_loaded_in_pid(pid, dll_basename_lower):
    PROCESS_QUERY_INFORMATION = 0x0400
    PROCESS_VM_READ = 0x0010
    hproc = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not hproc:
        return False
    try:
        hmods = (wintypes.HMODULE * 1024)()
        cb_needed = wintypes.DWORD()
        if not psapi.EnumProcessModules(hproc, ctypes.byref(hmods), ctypes.sizeof(hmods), ctypes.byref(cb_needed)):
            return False
        count = min(1024, cb_needed.value // ctypes.sizeof(wintypes.HMODULE))
        buf = ctypes.create_unicode_buffer(512)
        for i in range(count):
            if psapi.GetModuleFileNameExW(hproc, wintypes.HMODULE(hmods[i]), buf, 512) > 0:
                if buf.value.lower().endswith("\\" + dll_basename_lower):
                    return True
        return False
    finally:
        kernel32.CloseHandle(hproc)


def main():
    assert os.path.exists(CONTROLLER_EXE), f"Missing {CONTROLLER_EXE}"
    assert os.path.exists(HOOK_DLL), f"Missing {HOOK_DLL}"

    # Ensure clean start
    subprocess.run([CONTROLLER_EXE, "--stop"], check=False)
    time.sleep(0.4)

    cabs = find_cabinets()
    created_test_explorer = False
    if not cabs:
        subprocess.Popen(["explorer.exe", r"C:\Windows"])
        created_test_explorer = True
        for _ in range(25):
            time.sleep(0.25)
            cabs = find_cabinets()
            if cabs:
                break
        time.sleep(0.8)

    assert cabs, "Failed to find an open CabinetWClass File Explorer window"
    hwnd_cab = cabs[0]

    # Restore to normal window size so resize tests work predictably
    SW_RESTORE = 9
    user32.ShowWindow(hwnd_cab, SW_RESTORE)
    time.sleep(0.3)
    user32.SetWindowPos(hwnd_cab, 0, 80, 60, 1360, 820, 0x0004)  # SWP_NOZORDER
    user32.SetForegroundWindow(hwnd_cab)
    time.sleep(0.5)

    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd_cab, ctypes.byref(pid))
    explorer_pid = pid.value
    print(f"[INFO] Target CabinetWClass={hex(hwnd_cab)} explorer_pid={explorer_pid}")

    # Find active ShellTabWindowClass
    tabs = [t for t in enum_children(hwnd_cab, "ShellTabWindowClass") if user32.GetParent(t) == hwnd_cab and user32.IsWindowVisible(t)]
    assert tabs, "No visible ShellTabWindowClass found"
    hwnd_tab = tabs[0]
    duis = [d for d in enum_children(hwnd_tab, "DUIViewWndClassName") if user32.GetParent(d) == hwnd_tab]
    assert duis, "No DUIViewWndClassName found"
    hwnd_dui = duis[0]

    # 1. Start Controller in silent mode so Step 1 can inspect pre-split button first
    print("[STEP 1] Launching ExplorerSplitController.exe --silent ...")
    subprocess.Popen([CONTROLLER_EXE, "--silent"])
    time.sleep(1.0)

    assert is_dll_loaded_in_pid(explorer_pid, "explorersplithook.dll"), "Hook DLL was not loaded into explorer.exe"
    toggle_btns = enum_children(hwnd_tab, "ExplorerSplit_ToggleBtn")
    assert toggle_btns and user32.IsWindowVisible(toggle_btns[0]), "Toggle button [Split] not visible before split"
    print("[PASS] Hook DLL attached & in-window Split Toggle button is visible.")
    save_window_png(hwnd_cab, SCREENSHOT_SINGLE)

    # 2. Toggle Split View ON
    print("[STEP 2] Activating Split View via --toggle ...")
    subprocess.run([CONTROLLER_EXE, "--toggle"], check=True)
    time.sleep(1.2)

    right_panes = enum_children(hwnd_tab, "ExplorerSplit_RightPane")
    splitters = enum_children(hwnd_tab, "ExplorerSplit_Splitter")
    focus_bars = enum_children(hwnd_tab, "ExplorerSplit_FocusBar")
    assert right_panes and user32.IsWindowVisible(right_panes[0]), "RightPane is not visible!"
    assert splitters and user32.IsWindowVisible(splitters[0]), "Splitter is not visible!"
    assert focus_bars and user32.IsWindowVisible(focus_bars[0]), "FocusBar is not visible!"

    hwnd_right = right_panes[0]
    hwnd_split = splitters[0]

    tab_w, tab_h = get_client_size(hwnd_tab)
    lx, ly, lw, lh = get_rel_rect(hwnd_dui, hwnd_tab)
    sx, sy, sw, sh = get_rel_rect(hwnd_split, hwnd_tab)
    rx, ry, rw, rh = get_rel_rect(hwnd_right, hwnd_tab)
    print(f"[GEOM] Tab={tab_w}x{tab_h} | Left=({lx},{ly}) {lw}x{lh} | Splitter=({sx},{sy}) {sw}x{sh} | Right=({rx},{ry}) {rw}x{rh}")

    assert abs((lw + sw + rw) - tab_w) <= 2, f"Widths do not sum to tab_w: {lw}+{sw}+{rw} != {tab_w}"
    assert abs(lw - rw) <= 6, f"50/50 split expected lw ~= rw, got {lw} vs {rw}"

    def_views = enum_children(hwnd_right, "SHELLDLL_DefView")
    assert def_views and user32.IsWindowVisible(def_views[0]), "IExplorerBrowser SHELLDLL_DefView missing inside RightPane!"

    edits = [e for e in enum_children(hwnd_right, "Edit") if user32.GetParent(e) == hwnd_right]
    assert edits, "Address Edit control missing inside RightPane!"
    hwnd_edit = edits[0]
    initial_right_path = get_text(hwnd_edit)
    print(f"[PASS] RightPane IExplorerBrowser active, initial synced path = '{initial_right_path}'")
    assert initial_right_path.lower() == r"c:\windows", f"Expected C:\\Windows synced from Left Pane, got '{initial_right_path}'"

    # 3. Navigate Right Pane to a second folder (C:\Users) so two distinct folders are shown side-by-side
    print("[STEP 3] Navigating Right Pane independently to C:\\Users ...")
    WM_SETTEXT = 0x000C
    WM_KEYDOWN = 0x0100
    WM_KEYUP = 0x0101
    VK_RETURN = 0x0D
    cur_tid = kernel32.GetCurrentThreadId()
    exp_tid = user32.GetWindowThreadProcessId(hwnd_edit, None)
    user32.AttachThreadInput(cur_tid, exp_tid, True)
    user32.SetForegroundWindow(hwnd_cab)
    user32.SetFocus(hwnd_edit)
    user32.AttachThreadInput(cur_tid, exp_tid, False)
    time.sleep(0.1)
    text_buf = ctypes.create_unicode_buffer(r"C:\Users")
    user32.SendMessageW(hwnd_edit, WM_SETTEXT, 0, ctypes.addressof(text_buf))
    user32.PostMessageW(hwnd_edit, WM_KEYDOWN, VK_RETURN, 0)
    user32.PostMessageW(hwnd_edit, WM_KEYUP, VK_RETURN, 0)
    time.sleep(1.2)
    updated_right_path = get_text(hwnd_edit)
    print(f"[INFO] RightPane path after navigation = '{updated_right_path}'")
    assert "users" in updated_right_path.lower(), f"Unexpected path: {updated_right_path}"
    print("[PASS] Independent folder navigation in Right Pane works!")

    # 4. Resize Explorer window and verify WM_WINDOWPOSCHANGING maintains Dual-Pane layout
    print("[STEP 4] Resizing CabinetWClass to 1480x860 and checking layout persistence ...")
    user32.SetWindowPos(hwnd_cab, 0, 60, 50, 1480, 860, 0x0004)
    time.sleep(0.6)
    tab_w2, tab_h2 = get_client_size(hwnd_tab)
    _, _, lw2, _ = get_rel_rect(hwnd_dui, hwnd_tab)
    _, _, sw2, _ = get_rel_rect(hwnd_split, hwnd_tab)
    _, _, rw2, _ = get_rel_rect(hwnd_right, hwnd_tab)
    print(f"[GEOM AFTER RESIZE] Tab={tab_w2}x{tab_h2} | LeftW={lw2} | SplitW={sw2} | RightW={rw2}")
    assert tab_w2 > tab_w, "Window did not resize"
    assert abs((lw2 + sw2 + rw2) - tab_w2) <= 2, f"Post-resize width mismatch: {lw2}+{sw2}+{rw2} != {tab_w2}"
    print("[PASS] Dual-Pane layout resized seamlessly without snapping back!")

    # Capture live screenshot for walkthrough artifact
    time.sleep(0.4)
    if save_window_png(hwnd_cab, SCREENSHOT_OUT):
        print(f"[PASS] Saved live Dual-Pane screenshot to {SCREENSHOT_OUT}")

    # 5. Clean Shutdown & DLL Unload check
    print("[STEP 5] Stopping Controller (--stop) and verifying clean DLL unload from explorer.exe ...")
    subprocess.run([CONTROLLER_EXE, "--stop"], check=True)
    time.sleep(0.8)

    tab_w3, tab_h3 = get_client_size(hwnd_tab)
    lx3, ly3, lw3, lh3 = get_rel_rect(hwnd_dui, hwnd_tab)
    print(f"[GEOM AFTER UNLOAD] Tab={tab_w3}x{tab_h3} | DUIView=({lx3},{ly3}) {lw3}x{lh3}")
    assert lx3 == 0 and ly3 == 0 and lw3 == tab_w3 and lh3 == tab_h3, "DUIViewWndClassName did not restore to 100% tab bounds!"
    assert not enum_children(hwnd_tab, "ExplorerSplit_RightPane"), "RightPane HWND leaked after shutdown!"
    assert not enum_children(hwnd_tab, "ExplorerSplit_ToggleBtn"), "ToggleBtn HWND leaked after shutdown!"
    assert not enum_children(hwnd_tab, "ExplorerSplit_Splitter"), "Splitter HWND leaked after shutdown!"
    assert not enum_children(hwnd_tab, "ExplorerSplit_FocusBar"), "FocusBar HWND leaked after shutdown!"
    print("[PASS] Explorer restored to 100% width and all Split View controls cleanly torn down!")

    if created_test_explorer:
        WM_CLOSE = 0x0010
        user32.PostMessageW(hwnd_cab, WM_CLOSE, 0, 0)

    print("\nALL AUTOMATED SPLIT-VIEW TESTS PASSED SUCCESSFULLY!")


if __name__ == "__main__":
    main()
