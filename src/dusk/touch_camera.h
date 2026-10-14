#pragma once

namespace dusk::touch_camera {

constexpr float YAW_DEGREES_PER_DP = 0.34f;
constexpr float PITCH_DEGREES_PER_DP = 0.22f;

void add_delta(float yaw_dp, float pitch_dp) noexcept;
bool consume_delta(float& yaw_dp, float& pitch_dp) noexcept;
void clear() noexcept;
void get_pending(float& yaw_dp, float& pitch_dp) noexcept;
void set_pending(float yaw_dp, float pitch_dp) noexcept;

}  // namespace dusk::touch_camera
