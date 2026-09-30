// Separate TU: windows.h macros clash with decomp names.
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "autotest/Input.hpp"

#include <cstdint>

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

// SDL3 mouse events (SDL_events.h) for SDL_PushEvent: posted mouse messages never reach SDL.
constexpr uint32_t kSdlMouseMotion = 0x400;
constexpr uint32_t kSdlMouseButtonDown = 0x401;
constexpr uint32_t kSdlMouseButtonUp = 0x402;

struct SdlMouseMotion {
    uint32_t type, reserved;
    uint64_t timestamp;
    uint32_t windowID, which, state;
    float x, y, xrel, yrel;
};

struct SdlMouseButton {
    uint32_t type, reserved;
    uint64_t timestamp;
    uint32_t windowID, which;
    uint8_t button;
    bool down;
    uint8_t clicks, padding;
    float x, y;
};

union SdlEvent {
    SdlMouseMotion motion;
    SdlMouseButton button;
    uint8_t padding[128];
};

template <typename Fn>
Fn sdlProc(const char* name) {
    const HMODULE sdl = GetModuleHandleA("SDL3.dll");
    return sdl != nullptr ? reinterpret_cast<Fn>(GetProcAddress(sdl, name)) : nullptr;
}

uint32_t sdlWindowId() {
    using GetWindowsFn = void** (*)(int*);
    using GetWindowIdFn = uint32_t (*)(void*);
    using FreeFn = void (*)(void*);
    static const auto getWindows = sdlProc<GetWindowsFn>("SDL_GetWindows");
    static const auto getWindowId = sdlProc<GetWindowIdFn>("SDL_GetWindowID");
    static const auto sdlFree = sdlProc<FreeFn>("SDL_free");
    if (getWindows == nullptr || getWindowId == nullptr || sdlFree == nullptr) {
        return 0;
    }
    int count = 0;
    void** windows = getWindows(&count);
    const uint32_t id = windows != nullptr && count > 0 ? getWindowId(windows[0]) : 0;
    sdlFree(windows);
    return id;
}

bool pushSdlEvent(SdlEvent& event) {
    using PushEventFn = bool (*)(SdlEvent*);
    static const auto push = sdlProc<PushEventFn>("SDL_PushEvent");
    return push != nullptr && push(&event);
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

bool mouseMove(int x, int y, bool leftDown) {
    SdlEvent event{};
    event.motion = {kSdlMouseMotion, 0, 0, sdlWindowId(), 0, leftDown ? 1u : 0u,
        static_cast<float>(x), static_cast<float>(y), 0.0f, 0.0f};
    return pushSdlEvent(event);
}

bool mouseButton(int x, int y, bool down) {
    SdlEvent event{};
    event.button = {down ? kSdlMouseButtonDown : kSdlMouseButtonUp, 0, 0, sdlWindowId(), 0, 1, down,
        1, 0, static_cast<float>(x), static_cast<float>(y)};
    return pushSdlEvent(event);
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

bool mouseMove(int, int, bool) {
    return false;
}

bool mouseButton(int, int, bool) {
    return false;
}

#endif

}  // namespace twili::autotest::input
