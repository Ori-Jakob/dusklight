#pragma once

#include <SDL3/SDL_events.h>

namespace dusk::mouse {
void read();
void get_aim_deltas(float& out_yaw, float& out_pitch);
void get_camera_deltas(float& out_yaw, float& out_pitch);
void handle_event(const SDL_Event& event) noexcept;
void on_focus_lost();
void on_focus_gained();
// The host currently shows the mouse cursor (not captured, not hidden while idle).
bool cursor_shown() noexcept;
// Counts as cursor motion: UI under a resting cursor keeps it from hiding.
void keep_cursor_awake() noexcept;
}  // namespace dusk::mouse
