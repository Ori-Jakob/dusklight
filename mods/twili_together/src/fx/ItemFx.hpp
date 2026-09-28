#pragma once

// Weapons and items out in the world as the local player's own item actors show them

#include "presence/RemotePose.hpp"

#include <cstdint>

struct cXyz;

namespace twili::itemfx {

// At most this many events ride in one packet
inline constexpr int kMaxEventsPerPacket = 8;

// d_a_arrow.cpp setBombArrowExplode
void noteBombArrowExplode(const cXyz& pos, bool underwater);

// d_a_alink.cpp's hooks, for the local player only (they skip dummies).
void noteLevelSfx(uint32_t id, uint8_t kind, uint32_t mapInfo);
void noteParticle(uint16_t id, int count, const cXyz& pos, int16_t rotX, int16_t rotY,
                  float scale = 1.0f);
void noteHitMark(uint16_t type, const cXyz& pos, int16_t rotX, int16_t rotY);
void noteSound(uint32_t id, const cXyz& pos, uint32_t mapInfo);
void noteWater(const cXyz& pos, float scale);

// The local player's items this tick, read right after its execute() (sendPlayerUpdate).
int captureLocal(uint32_t seq, RemoteItemFx& out, ItemFxEvent events[kMaxEventsPerPacket]);
// Our PLAYER_UPDATE vf bits of the items
uint16_t localVisFlags();

void encodeSlot(const ItemFxSlot& s, int32_t x[9]);
// Kind, state and flags masked to what the dummy knows
ItemFxSlot decodeSlot(const int32_t x[9]);
// The daAlink groups.
void encodeHookshot(const RemoteHookshot& h, int32_t out[11]);
RemoteHookshot decodeHookshot(const int32_t in[11]);
void encodeIronBall(const RemoteIronBall& b, int32_t out[7]);
RemoteIronBall decodeIronBall(const int32_t in[7]);
void encodeLevelSfx(const RemoteLevelSfx (&ls)[kItemFxLevelSfx], int32_t out[8]);
void decodeLevelSfx(const int32_t in[8], RemoteLevelSfx (&ls)[kItemFxLevelSfx]);
void encodeHeldExtra(const RemoteHeldExtra& x, int32_t out[4]);
RemoteHeldExtra decodeHeldExtra(const int32_t in[4]);
void encodeEvent(const ItemFxEvent& ev, int32_t out[7]);
// False for an event the dummy does not know.
bool decodeEvent(const int32_t in[7], ItemFxEvent& out);

// What the capture did, for the autotest.
struct SenderStats {
    uint32_t objects[kItemFxKindCount] = {};  // new objects given a slot, per kind
    uint32_t events[kItemFxEvTypeCount] = {};  // events sent, per type
    uint32_t evicted = 0;                      // stuck arrows dropped for a moving object
    uint32_t hookOutTicks = 0;                 // captures with a clawshot tip out
    uint32_t ironBallTicks = 0;                // captures with the ball out of the hand
    uint32_t levelSfxTicks = 0;                // captures with a level sound
    uint32_t levelSfxDropped = 0;              // level sounds past kItemFxLevelSfx in a tick
    float hookMaxDist = 0.0f;                  // the farthest a tip got from its hand
};
const SenderStats& senderStats();
const RemoteItemFx& lastCaptured();
void injectForTest(const RemoteItemFx& slots, int ticks, const ItemFxEvent* events, int count);

}  // namespace twili::itemfx
