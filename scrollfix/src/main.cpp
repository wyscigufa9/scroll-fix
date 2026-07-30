/*
 * ScrollFix - mouse-wheel direction reversal filter for Windows.
 *
 * Copyright (c) 2026 wyscigufa9.
 * Licensed under the MIT License. See LICENSE for details.
 */

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

#include "resource.h"

namespace {

constexpr wchar_t kAppName[] = L"ScrollFix";
constexpr wchar_t kWindowClass[] = L"ScrollFix.HiddenWindow";
constexpr wchar_t kMutexName[] = L"Local\\ScrollFix.SingleInstance";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kMenuEnabled = 1001;
constexpr UINT kMenuReload = 1002;
constexpr UINT kMenuOpenConfig = 1003;
constexpr UINT kMenuAutostart = 1004;
constexpr UINT kMenuExit = 1005;

struct Config {
    bool enabled = true;
    bool filterVertical = true;
    bool filterHorizontal = true;
    bool strictGestureLock = true;
    bool blockMiddleButton = false;
    bool bypassWithCtrl = true;
    bool bypassWithAlt = true;
    DWORD scoreResetMs = 220;
    unsigned directionSwitchScore = 360;
    unsigned maximumEventDelta = 960;
};

struct AxisState {
    int acceptedDirection = 0;
    int gestureScore = 0;
    ULONGLONG lastInputAt = 0;

    void reset() noexcept { *this = {}; }
};

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HHOOK g_mouseHook = nullptr;
HANDLE g_mutex = nullptr;
UINT g_taskbarCreated = 0;
Config g_config;
AxisState g_vertical;
AxisState g_horizontal;
std::filesystem::path g_configPath;
std::filesystem::path g_executablePath;

int ReadInt(
    const wchar_t* key,
    int fallback,
    int minimum,
    int maximum) {
    const auto value = GetPrivateProfileIntW(
        L"filter", key, fallback, g_configPath.c_str());
    return std::clamp<int>(
        static_cast<int>(value), minimum, maximum);
}

bool ReadBool(const wchar_t* key, bool fallback) {
    return ReadInt(key, fallback ? 1 : 0, 0, 1) != 0;
}

void ResetFilterState() {
    g_vertical.reset();
    g_horizontal.reset();
}

bool EnsureConfigExists() {
    std::error_code error;
    std::filesystem::create_directories(g_configPath.parent_path(), error);
    if (std::filesystem::exists(g_configPath, error)) {
        return true;
    }

    std::ofstream file(g_configPath, std::ios::binary);
    if (!file) {
        return false;
    }

    file <<
        "; ScrollFix configuration\n"
        "; Values are reloaded from the tray menu. Times are milliseconds.\n"
        "[filter]\n"
        "enabled=1\n"
        "filter_vertical=1\n"
        "filter_horizontal=1\n"
        "strict_gesture_lock=1\n"
        "score_reset_ms=220\n"
        "block_middle_button=0\n"
        "bypass_with_ctrl=1\n"
        "bypass_with_alt=1\n"
        "configuration_version=13\n"
        "direction_switch_score=360\n"
        "maximum_event_delta=960\n";
    return file.good();
}

void LoadConfig() {
    EnsureConfigExists();
    std::array<wchar_t, 32> versionText{};
    const bool needsV13Upgrade =
        GetPrivateProfileStringW(
            L"filter", L"configuration_version", L"", versionText.data(),
            static_cast<DWORD>(versionText.size()), g_configPath.c_str()) == 0 ||
        _wtoi(versionText.data()) < 13;
    if (needsV13Upgrade) {
        WritePrivateProfileStringW(
            L"filter", L"strict_gesture_lock", L"1",
            g_configPath.c_str());
        WritePrivateProfileStringW(
            L"filter", L"score_reset_ms", L"220", g_configPath.c_str());
        WritePrivateProfileStringW(
            L"filter", L"block_middle_button", L"0", g_configPath.c_str());
        WritePrivateProfileStringW(
            L"filter", L"direction_switch_score", L"360",
            g_configPath.c_str());
        WritePrivateProfileStringW(
            L"filter", L"maximum_event_delta", L"960", g_configPath.c_str());
        WritePrivateProfileStringW(
            L"filter", L"configuration_version", L"13",
            g_configPath.c_str());
        // Remove settings belonging to older filter implementations during
        // the one-time upgrade only.
        constexpr const wchar_t* obsoleteKeys[] = {
            L"reversal_guard_ms",
            L"confirmation_window_ms",
            L"confirmation_min_duration_ms",
            L"confirmation_events",
            L"confirmation_delta",
            L"reversal_debounce_ms",
            L"idle_reset_ms",
            L"initial_confirmation_events",
            L"gesture_timeout_ms",
            L"reversal_window_ms",
            L"switch_to_up_score",
            L"switch_to_down_score",
            L"initial_direction_score",
        };
        for (const auto* key : obsoleteKeys) {
            WritePrivateProfileStringW(
                L"filter", key, nullptr, g_configPath.c_str());
        }
    }

    Config updated;
    updated.enabled = ReadBool(L"enabled", true);
    updated.filterVertical = ReadBool(L"filter_vertical", true);
    updated.filterHorizontal = ReadBool(L"filter_horizontal", true);
    updated.strictGestureLock = ReadBool(L"strict_gesture_lock", true);
    updated.blockMiddleButton = ReadBool(L"block_middle_button", false);
    updated.bypassWithCtrl = ReadBool(L"bypass_with_ctrl", true);
    updated.bypassWithAlt = ReadBool(L"bypass_with_alt", true);
    updated.scoreResetMs = static_cast<DWORD>(
        ReadInt(L"score_reset_ms", 220, 30, 2000));
    updated.directionSwitchScore = static_cast<unsigned>(
        ReadInt(L"direction_switch_score", 360, 120, 4800));
    updated.maximumEventDelta =
        static_cast<unsigned>(ReadInt(L"maximum_event_delta", 960, 120, 9600));
    g_config = updated;
    ResetFilterState();
}

bool ModifierDown(int virtualKey) {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

bool ShouldSuppress(AxisState& state, int delta, ULONGLONG now) {
    const int direction = delta > 0 ? 1 : -1;
    const ULONGLONG gap =
        state.lastInputAt == 0 ? std::numeric_limits<ULONGLONG>::max()
                               : now - state.lastInputAt;
    state.lastInputAt = now;

    if (gap > g_config.scoreResetMs) {
        state.gestureScore = 0;
    }

    const long long scoreLimit =
        static_cast<long long>(g_config.directionSwitchScore) * 4;
    state.gestureScore = static_cast<int>(std::clamp<long long>(
        static_cast<long long>(state.gestureScore) + delta,
        -scoreLimit, scoreLimit));

    if (state.acceptedDirection == 0) {
        state.acceptedDirection = direction;
        return false;
    }

    if (direction == state.acceptedDirection) {
        return false;
    }

    const int scoreDirection =
        state.gestureScore > 0 ? 1 : state.gestureScore < 0 ? -1 : 0;
    const bool scoreConfirmsDirection =
        scoreDirection == direction &&
        static_cast<unsigned>(std::abs(state.gestureScore)) >=
            g_config.directionSwitchScore;
    if (!g_config.strictGestureLock || scoreConfirmsDirection) {
        state.acceptedDirection = direction;
        state.gestureScore =
            direction * static_cast<int>(g_config.directionSwitchScore);
        return false;
    }

    return true;
}

LRESULT CALLBACK MouseHook(int code, WPARAM messageValue, LPARAM data) {
    if (code != HC_ACTION) {
        return CallNextHookEx(g_mouseHook, code, messageValue, data);
    }

    const UINT message = static_cast<UINT>(messageValue);
    const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
    const bool middle =
        message == WM_MBUTTONDOWN || message == WM_MBUTTONUP;
    const bool wheel =
        message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
    if (!middle && !wheel) {
        return CallNextHookEx(g_mouseHook, code, messageValue, data);
    }

    if ((event->flags & LLMHF_INJECTED) != 0) {
        return CallNextHookEx(g_mouseHook, code, messageValue, data);
    }

    if (!g_config.enabled) {
        return CallNextHookEx(g_mouseHook, code, messageValue, data);
    }

    if (middle) {
        if (g_config.blockMiddleButton) {
            return 1;
        }
        return CallNextHookEx(g_mouseHook, code, messageValue, data);
    }

    if (
        (g_config.bypassWithCtrl && ModifierDown(VK_CONTROL)) ||
        (g_config.bypassWithAlt && ModifierDown(VK_MENU))) {
        return CallNextHookEx(g_mouseHook, code, messageValue, data);
    }

    const bool horizontal = message == WM_MOUSEHWHEEL;
    if ((horizontal && !g_config.filterHorizontal) ||
        (!horizontal && !g_config.filterVertical)) {
        return CallNextHookEx(g_mouseHook, code, messageValue, data);
    }

    const int rawDelta = static_cast<short>(HIWORD(event->mouseData));
    if (rawDelta == 0) {
        return CallNextHookEx(g_mouseHook, code, messageValue, data);
    }

    const auto rawMagnitude = static_cast<unsigned>(
        rawDelta < 0 ? -static_cast<long long>(rawDelta) : rawDelta);
    AxisState& state = horizontal ? g_horizontal : g_vertical;

    // Malformed/coalesced deltas can cause an application to jump a very long
    // distance. Dropping them is safer than injecting a replacement event.
    if (rawMagnitude > g_config.maximumEventDelta) {
        return 1;
    }

    if (ShouldSuppress(state, rawDelta, GetTickCount64())) {
        return 1;
    }

    return CallNextHookEx(g_mouseHook, code, messageValue, data);
}

bool IsAutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    const auto closeKey = [&] { RegCloseKey(key); };

    DWORD type = 0;
    DWORD bytes = 0;
    const LONG result =
        RegQueryValueExW(key, kAppName, nullptr, &type, nullptr, &bytes);
    closeKey();
    return result == ERROR_SUCCESS && type == REG_SZ && bytes > sizeof(wchar_t);
}

bool SetAutostart(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
            &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command =
            L"\"" + g_executablePath.wstring() + L"\" --autostart";
        result = RegSetValueExW(
            key, kAppName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kAppName);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

NOTIFYICONDATAW TrayData(DWORD flags) {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = g_window;
    icon.uID = 1;
    icon.uFlags = flags;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(icon.szTip, g_config.enabled ? L"ScrollFix - enabled"
                                          : L"ScrollFix - paused");
    return icon;
}

void AddTrayIcon() {
    auto icon = TrayData(NIF_MESSAGE | NIF_ICON | NIF_TIP);
    Shell_NotifyIconW(NIM_ADD, &icon);
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);
}

void UpdateTrayIcon() {
    auto icon = TrayData(NIF_ICON | NIF_TIP);
    Shell_NotifyIconW(NIM_MODIFY, &icon);
}

void RemoveTrayIcon() {
    auto icon = TrayData(0);
    Shell_NotifyIconW(NIM_DELETE, &icon);
}

void ShowTrayMenu() {
    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(
        menu, MF_STRING | (g_config.enabled ? MF_CHECKED : 0), kMenuEnabled,
        L"Filtering enabled");
    AppendMenuW(menu, MF_STRING, kMenuReload, L"Reload configuration");
    AppendMenuW(menu, MF_STRING, kMenuOpenConfig, L"Open configuration");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0),
        kMenuAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    SetForegroundWindow(g_window);
    TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, point.x,
        point.y, 0, g_window, nullptr);
    DestroyMenu(menu);
}

void OpenConfig() {
    EnsureConfigExists();
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        g_window, L"open", g_configPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        ShellExecuteW(
            g_window, L"open", L"notepad.exe", g_configPath.c_str(), nullptr,
            SW_SHOWNORMAL);
    }
}

void HandleCommand(WORD command) {
    switch (command) {
    case kMenuEnabled:
        g_config.enabled = !g_config.enabled;
        ResetFilterState();
        WritePrivateProfileStringW(
            L"filter", L"enabled", g_config.enabled ? L"1" : L"0",
            g_configPath.c_str());
        UpdateTrayIcon();
        break;
    case kMenuReload:
        LoadConfig();
        UpdateTrayIcon();
        break;
    case kMenuOpenConfig:
        OpenConfig();
        break;
    case kMenuAutostart:
        if (!SetAutostart(!IsAutostartEnabled())) {
            MessageBoxW(
                g_window, L"Windows could not update your startup setting.",
                kAppName, MB_OK | MB_ICONERROR);
        }
        break;
    case kMenuExit:
        DestroyWindow(g_window);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK WindowProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_taskbarCreated) {
        AddTrayIcon();
        return 0;
    }

    switch (message) {
    case WM_COMMAND:
        HandleCommand(LOWORD(wParam));
        return 0;
    case kTrayMessage:
        switch (LOWORD(lParam)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            ShowTrayMenu();
            break;
        case WM_LBUTTONDBLCLK:
            g_config.enabled = !g_config.enabled;
            ResetFilterState();
            WritePrivateProfileStringW(
                L"filter", L"enabled", g_config.enabled ? L"1" : L"0",
                g_configPath.c_str());
            UpdateTrayIcon();
            break;
        default:
            break;
        }
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

bool HasArgument(std::wstring_view wanted) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) {
        return false;
    }
    bool found = false;
    for (int index = 1; index < count; ++index) {
        if (_wcsicmp(arguments[index], wanted.data()) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(arguments);
    return found;
}

bool InitializePaths() {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return false;
    }
    g_executablePath = std::wstring(executable.data(), length);

    std::array<wchar_t, 32768> localAppData{};
    const DWORD dataLength = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData.data(),
        static_cast<DWORD>(localAppData.size()));
    if (dataLength == 0 || dataLength >= localAppData.size()) {
        g_configPath = g_executablePath.parent_path() / L"config.ini";
    } else {
        g_configPath = std::filesystem::path(localAppData.data()) /
                       L"ScrollFix" / L"config.ini";
    }
    return true;
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    if (!InitializePaths()) {
        return 1;
    }

    if (HasArgument(L"--install-autostart")) {
        return SetAutostart(true) ? 0 : 1;
    }
    if (HasArgument(L"--remove-autostart")) {
        return SetAutostart(false) ? 0 : 1;
    }

    g_mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_mutex) {
            CloseHandle(g_mutex);
        }
        return 0;
    }

    LoadConfig();
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&windowClass)) {
        CloseHandle(g_mutex);
        return 1;
    }

    g_window = CreateWindowExW(
        0, kWindowClass, kAppName, WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
        instance, nullptr);
    if (!g_window) {
        CloseHandle(g_mutex);
        return 1;
    }

    g_mouseHook =
        SetWindowsHookExW(WH_MOUSE_LL, MouseHook, instance, 0);
    if (!g_mouseHook) {
        MessageBoxW(
            nullptr, L"ScrollFix could not install its mouse hook.", kAppName,
            MB_OK | MB_ICONERROR);
        DestroyWindow(g_window);
        CloseHandle(g_mutex);
        return 1;
    }

    AddTrayIcon();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
    CloseHandle(g_mutex);
    g_mutex = nullptr;
    return static_cast<int>(message.wParam);
}
