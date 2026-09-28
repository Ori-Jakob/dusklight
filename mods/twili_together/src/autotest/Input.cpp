// Separate TU: windows.h macros clash with decomp names.
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "autotest/Input.hpp"

namespace twili::autotest::input {

#if defined(_WIN32)
namespace {

struct Found {
    DWORD pid = 0;
    HWND sdl = nullptr;
    HWND any = nullptr;
};

BOOL CALLBACK findWindow(HWND hwnd, LPARAM param) {
    auto* found = reinterpret_cast<Found*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != found->pid || !IsWindowVisible(hwnd)) {
        return TRUE;
    }
    char cls[32] = {};
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (lstrcmpA(cls, "SDL_app") == 0) {
        found->sdl = hwnd;
        return FALSE;
    }
    if (found->any == nullptr) {
        found->any = hwnd;
    }
    return TRUE;
}

HWND gameWindow() {
    static HWND s_hwnd = nullptr;
    if (s_hwnd == nullptr || !IsWindow(s_hwnd)) {
        Found found{GetCurrentProcessId()};
        EnumWindows(findWindow, reinterpret_cast<LPARAM>(&found));
        s_hwnd = found.sdl != nullptr ? found.sdl : found.any;
    }
    return s_hwnd;
}

struct KeyInfo {
    UINT vk;
    bool extended;
};

KeyInfo keyInfo(Key key) {
    switch (key) {
    case Key::Left: return {VK_LEFT, true};
    case Key::Right: return {VK_RIGHT, true};
    case Key::Up: return {VK_UP, true};
    case Key::Down: return {VK_DOWN, true};
    case Key::Confirm: return {VK_RETURN, false};
    case Key::Cancel: return {VK_ESCAPE, false};
    case Key::Next: return {VK_NEXT, true};
    case Key::Prev: return {VK_PRIOR, true};
    }
    return {0, false};
}

// The scan code SDL maps the key from; the extended bit tells the arrows from the keypad.
LPARAM keyParam(const KeyInfo& info, bool up, bool repeat) {
    const UINT scan = MapVirtualKeyA(info.vk, MAPVK_VK_TO_VSC);
    LPARAM param = 1 | static_cast<LPARAM>(scan & 0xFF) << 16;
    if (info.extended) param |= LPARAM{1} << 24;
    if (repeat || up) param |= LPARAM{1} << 30;
    if (up) param |= LPARAM{1} << 31;
    return param;
}

}  // namespace

bool keyDown(Key key, bool repeat) {
    const HWND hwnd = gameWindow();
    const KeyInfo info = keyInfo(key);
    return hwnd != nullptr && PostMessageA(hwnd, WM_KEYDOWN, info.vk, keyParam(info, false, repeat));
}

bool keyUp(Key key) {
    const HWND hwnd = gameWindow();
    const KeyInfo info = keyInfo(key);
    return hwnd != nullptr && PostMessageA(hwnd, WM_KEYUP, info.vk, keyParam(info, true, false));
}

bool resizeWindow(int width, int height) {
    const HWND hwnd = gameWindow();
    if (hwnd == nullptr) {
        return false;
    }
    RECT rect = {0, 0, width, height};
    AdjustWindowRectEx(&rect, static_cast<DWORD>(GetWindowLongPtrA(hwnd, GWL_STYLE)), FALSE,
        static_cast<DWORD>(GetWindowLongPtrA(hwnd, GWL_EXSTYLE)));
    return SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
}

#else

bool keyDown(Key, bool) {
    return false;
}

bool keyUp(Key) {
    return false;
}

bool resizeWindow(int, int) {
    return false;
}

#endif

}  // namespace twili::autotest::input
