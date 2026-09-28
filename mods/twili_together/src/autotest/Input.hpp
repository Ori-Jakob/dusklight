#pragma once

// Keyboard input and window size for the game's own window, posted as window messages so the
// host UI sees ordinary key presses. Windows only; the others report false.
namespace twili::autotest::input {

enum class Key { Left, Right, Up, Down, Confirm, Cancel, Next, Prev };

bool keyDown(Key key, bool repeat);
bool keyUp(Key key);
// Client area size in pixels.
bool resizeWindow(int width, int height);

}  // namespace twili::autotest::input
