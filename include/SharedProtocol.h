#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <cwctype>
#include <string>

namespace ExplorerSplit {

// Registered Window Message names shared between Controller and Hook DLL
constexpr const wchar_t* kMsgNameInit       = L"ExplorerSplitView_MsgInit_v1";
constexpr const wchar_t* kMsgNameToggle     = L"ExplorerSplitView_MsgToggle_v1";
constexpr const wchar_t* kMsgNameResetRatio = L"ExplorerSplitView_MsgResetRatio_v1";
constexpr const wchar_t* kMsgNameShutdown   = L"ExplorerSplitView_MsgShutdown_v1";
constexpr const wchar_t* kMsgNameQueryState = L"ExplorerSplitView_MsgQueryState_v1";

// Window class names created inside ShellTabWindowClass
constexpr const wchar_t* kClsRightPane  = L"ExplorerSplit_RightPane";
constexpr const wchar_t* kClsSplitter   = L"ExplorerSplit_Splitter";
constexpr const wchar_t* kClsFocusBar   = L"ExplorerSplit_FocusBar";
constexpr const wchar_t* kClsToggleBtn  = L"ExplorerSplit_ToggleBtn";

// Single-instance mutex prefix for Controller (scoped per desktop)
constexpr const wchar_t* kControllerMutexPrefix = L"Local\\ExplorerSplitView_Controller_Mutex_";
constexpr const wchar_t* kControllerWndClass    = L"ExplorerSplit_ControllerHiddenWnd";

inline std::wstring GetDesktopScopedMutexName() {
    std::wstring name(kControllerMutexPrefix);
    HDESK hDesk = GetThreadDesktop(GetCurrentThreadId());
    wchar_t deskName[128]{};
    DWORD needed = 0;
    if (hDesk && GetUserObjectInformationW(hDesk, UOI_NAME, deskName, sizeof(deskName), &needed) && deskName[0] != L'\0') {
        for (wchar_t* p = deskName; *p; ++p) {
            if (*p == L'\\' || *p == L'/') *p = L'_';
        }
        name += deskName;
    } else {
        name += L"Default";
    }
    return name;
}

struct RegisteredMessages {
    UINT msgInit       = 0;
    UINT msgToggle     = 0;
    UINT msgResetRatio = 0;
    UINT msgShutdown   = 0;
    UINT msgQueryState = 0;

    void Initialize() {
        if (msgInit == 0) {
            msgInit       = RegisterWindowMessageW(kMsgNameInit);
            msgToggle     = RegisterWindowMessageW(kMsgNameToggle);
            msgResetRatio = RegisterWindowMessageW(kMsgNameResetRatio);
            msgShutdown   = RegisterWindowMessageW(kMsgNameShutdown);
            msgQueryState = RegisterWindowMessageW(kMsgNameQueryState);
        }
    }
};

inline int ScaleForDpi(int px, UINT dpi) {
    if (dpi == 0) dpi = 96;
    return MulDiv(px, static_cast<int>(dpi), 96);
}

inline UINT SafeGetDpiForWindow(HWND hwnd) {
    if (hwnd && IsWindow(hwnd)) {
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi > 0) return dpi;
    }
    return 96;
}

inline bool IsSystemDarkMode() {
    DWORD useLightTheme = 1;
    DWORD cbData = sizeof(useLightTheme);
    LSTATUS st = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &useLightTheme,
        &cbData
    );
    if (st == ERROR_SUCCESS) {
        return (useLightTheme == 0);
    }
    return false;
}

inline COLORREF GetSystemAccentColor(bool darkMode) {
    // Prefer Windows 11 Explorer AccentPalette:
    // Index 1 (bytes 4..6)  = SystemAccentColorLight2 (ideal for Dark Mode)
    // Index 4 (bytes 16..18)= SystemAccentColorDark1  (ideal for Light Mode)
    BYTE palette[32]{};
    DWORD cbPalette = sizeof(palette);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
            L"AccentPalette",
            RRF_RT_REG_BINARY,
            nullptr,
            palette,
            &cbPalette
        ) == ERROR_SUCCESS && cbPalette >= 24) {
        size_t offset = darkMode ? 4 : 16;
        BYTE r = palette[offset + 0];
        BYTE g = palette[offset + 1];
        BYTE b = palette[offset + 2];
        int maxCh = std::max({ static_cast<int>(r), static_cast<int>(g), static_cast<int>(b) });
        if (!darkMode || maxCh >= 135) {
            return RGB(r, g, b);
        }
    }

    DWORD abgr = 0;
    DWORD cbData = sizeof(abgr);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\DWM",
            L"AccentColor",
            RRF_RT_REG_DWORD,
            nullptr,
            &abgr,
            &cbData
        ) == ERROR_SUCCESS && abgr != 0) {
        BYTE r = static_cast<BYTE>(abgr & 0xFF);
        BYTE g = static_cast<BYTE>((abgr >> 8) & 0xFF);
        BYTE b = static_cast<BYTE>((abgr >> 16) & 0xFF);
        int maxCh = std::max({ static_cast<int>(r), static_cast<int>(g), static_cast<int>(b) });
        if (!darkMode || maxCh >= 135) {
            return RGB(r, g, b);
        }
    }

    return darkMode ? RGB(76, 194, 255) : RGB(0, 103, 192);
}

struct ThemePalette {
    bool dark = false;
    COLORREF headerBg       = RGB(243, 243, 243);
    COLORREF paneBg         = RGB(255, 255, 255);
    COLORREF editBg         = RGB(255, 255, 255);
    COLORREF editBorder     = RGB(215, 215, 215);
    COLORREF editFocusBorder= RGB(0, 103, 192);
    COLORREF textPrimary    = RGB(26, 26, 26);
    COLORREF textSecondary  = RGB(96, 96, 96);
    COLORREF btnHoverBg     = RGB(229, 229, 229);
    COLORREF btnPressBg     = RGB(213, 213, 213);
    COLORREF closeHoverBg   = RGB(196, 43, 28);
    COLORREF splitterBg     = RGB(238, 238, 238);
    COLORREF splitterHoverBg= RGB(224, 224, 224);
    COLORREF splitterGrip   = RGB(150, 150, 150);
    COLORREF inactiveBar    = RGB(224, 224, 224);
    COLORREF accent         = RGB(0, 103, 192);

    static ThemePalette Current() {
        ThemePalette p{};
        p.dark = IsSystemDarkMode();
        p.accent = GetSystemAccentColor(p.dark);
        p.editFocusBorder = p.accent;

        if (p.dark) {
            p.headerBg        = RGB(32, 32, 32);
            p.paneBg          = RGB(25, 25, 25);
            p.editBg          = RGB(43, 43, 43);
            p.editBorder      = RGB(62, 62, 62);
            p.textPrimary     = RGB(245, 245, 245);
            p.textSecondary   = RGB(175, 175, 175);
            p.btnHoverBg      = RGB(52, 52, 52);
            p.btnPressBg      = RGB(66, 66, 66);
            p.closeHoverBg    = RGB(196, 43, 28);
            p.splitterBg      = RGB(30, 30, 30);
            p.splitterHoverBg = RGB(48, 48, 48);
            p.splitterGrip    = RGB(120, 120, 120);
            p.inactiveBar     = RGB(45, 45, 45);
        }
        return p;
    }
};

inline bool IsExplorerProcessId(DWORD pid) {
    if (pid == 0) return false;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    wchar_t path[MAX_PATH * 2]{};
    DWORD len = static_cast<DWORD>(std::size(path));
    bool match = false;
    if (QueryFullProcessImageNameW(hProc, 0, path, &len) && len > 0) {
        std::wstring s(path, len);
        for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
        const std::wstring suffix = L"\\explorer.exe";
        if (s.size() >= suffix.size() &&
            s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0) {
            match = true;
        }
    }
    CloseHandle(hProc);
    return match;
}

} // namespace ExplorerSplit
