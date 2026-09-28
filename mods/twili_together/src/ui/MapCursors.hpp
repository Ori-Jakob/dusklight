#pragma once

#include "dolphin/types.h"

#include <cstdint>
#include <string>
#include <vector>

class dMap_c;
class dMenuMapCommon_c;

// Remote players on the maps, as direct-colour triangles over the finished picture.
namespace twili::ui::map_cursor {

enum class Surface : uint8_t { Minimap = 0, PauseDmap = 1, Count };

// Why a frame drew no marker at all. Only Drawn ran the per-client filters.
enum class Gate : uint8_t {
    Drawn,
    AlphaZero,
    NotConnected,
    LocationsOff,
    CutsceneHidden,
    NoStage,
};

// Why a known client got no marker in a frame that ran the filters.
enum class Skip : uint8_t { NoSave, NoUpdate, OtherStage, OtherLayer, OtherFloor, BadPose };

const char* gateName(Gate g);
const char* skipName(Skip s);

struct Vec2 {
    f32 x = 0.0f, y = 0.0f;
};

// The minimap offscreen transform as this frame's renderingMap() used it.
struct MinimapXform {
    f32 centerX = 0.0f, centerZ = 0.0f;
    f32 worldW = 1.0f, worldH = 1.0f;  // cm covered by the texture
    u16 texW = 1, texH = 1;            // logical size
    bool mirror = false;
    f32 rectX = 0.0f, rectY = 0.0f, rectW = 0.0f, rectH = 0.0f;  // the picture, in J2D units
    f32 cursorSize = 0.0f;  // renderingAmap_c::getPlayerCursorSize(), in texels
};

// A position on the map, room-origin corrected like dMapInfo_n::getMapPlayerPos.
struct MapPose {
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    s16 angle = 0;
    int8_t roomNo = -1;
};

// One marker exactly as it was sent to GX.
struct Drawn {
    uint32_t clientId = 0;
    MapPose pose;
    Vec2 anchor;  // clamped to the rect edge when pinned
    Vec2 dir;     // unit facing on screen
    Vec2 tip;
    f32 height = 0.0f;  // tip-to-base, J2D units
    u8 fill[4] = {};
    u8 outline[4] = {};
    bool pinned = false;    // outside the map area, drawn at its edge pointing towards it
    int8_t floorDelta = 0;  // dungeon floor relative to the one shown
    bool injected = false;  // an autotest client
};

struct Skipped {
    uint32_t clientId = 0;
    Skip reason = Skip::NoSave;
};

struct Frame {
    Surface surface = Surface::Minimap;
    bool valid = false;
    uint32_t tick = 0;  // g_Counter.mCounter0 when drawn
    Gate gate = Gate::Drawn;
    MinimapXform xform;  // minimap only
    MapPose local;
    Vec2 localAnchor;
    Vec2 localDir;
    f32 localHeight = 0.0f;
    bool localRedrawn = false;  // our arrow drawn again over an overlapping marker
    u8 mapAlpha = 0;
    bool rectOnScreen = false;
    std::vector<Drawn> cursors;
    std::vector<Skipped> skipped;
};

// After the minimap picture, with its rect and alpha.
void drawMinimap(dMap_c& map, f32 x, f32 y, f32 w, f32 h, u8 alpha);

// Pause dungeon map: `cnv` maps onto the pane; called per blended floor with its weight.
using PauseCnvFn = void (*)(const void* ctx, f32 x, f32 z, f32* px, f32* py);
void collectPauseDmap(s8 floorNo, f32 alphaRate, PauseCnvFn cnv, const void* ctx);
// Just before the menu's own icons, with drawIcon's arguments.
void drawPauseDmap(dMenuMapCommon_c& common, f32 originX, f32 originY, f32 alpha);

const Frame& lastFrame(Surface s);

// Autotest clients drawn as if they were remote players. Positions are map positions.
struct TestClient {
    uint32_t clientId = 0;
    u8 r = 255, g = 255, b = 255;
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    s16 angle = 0;
    int8_t roomNo = -1;
};
void setTestClients(std::vector<TestClient> clients);

Vec2 projectMinimap(const MinimapXform& t, f32 x, f32 z);
Vec2 facingMinimap(const MinimapXform& t, s16 angle);
// Projection, outline, pins, colours; false with `failure` set.
bool selfTest(std::string& failure);

void reset();

}  // namespace twili::ui::map_cursor
