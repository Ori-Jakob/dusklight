#pragma once

// Key presses and window size posted to the game's own window (Windows only).
namespace twili::autotest::input {

enum class Key { Left, Right, Up, Down, Confirm, Cancel, Next, Prev };

bool keyDown(Key key, bool repeat);
bool keyUp(Key key);
// Client area size in pixels.
bool resizeWindow(int width, int height);

}  // namespace twili::autotest::input
