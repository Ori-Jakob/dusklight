#pragma once

#include <cstddef>
#include <cstdint>

// The warp tool's stages, rooms and PLYR points; the host keeps its copy internal.
namespace twili::maps {

struct Room {
    const char* name;
    const char* stage;
    int8_t room;  // -1: the entry names the stage only
    const int16_t* points;
    uint8_t pointCount;
};

extern const Room kRooms[];
extern const size_t kRoomCount;

}  // namespace twili::maps
