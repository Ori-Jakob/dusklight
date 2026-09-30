#pragma once

// Key presses, mouse input and window size for the game's own window (Windows only).
namespace twili::autotest::input {

enum class Key { Left, Right, Up, Down, Confirm, Cancel, Next, Prev };

bool keyDown(Key key, bool repeat);
bool keyUp(Key key);
// Client area size in pixels.
bool resizeWindow(int width, int height);
// SDL mouse events at window coordinates x, y: a move with the left button held or not, or a
// left button press or release.
bool mouseMove(int x, int y, bool leftDown);
bool mouseButton(int x, int y, bool down);

}  // namespace twili::autotest::input
