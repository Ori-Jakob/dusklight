#pragma once

#include <mods/api.h>

// Everything on screen: the window, styles, toasts, name tags and map cursors.
namespace twili::ui {

// mod_initialize; failures only disable the part that failed.
void init();
// Once per tick after the session.
void tick();
void shutdown();

}  // namespace twili::ui
