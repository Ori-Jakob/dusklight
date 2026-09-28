#pragma once

#include "presence/RemotePose.hpp"

#include "d/d_bg_s_gnd_chk.h"
#include "d/d_kankyo.h"
#include "m_Do/m_Do_ext.h"
#include "Z2AudioLib/Z2SoundObject.h"

class J3DAnmTevRegKey;
class J3DAnmTextureSRTKey;
class J3DAnmTransform;
class J3DJoint;
class J3DModel;

namespace twili {

class daDummyPlayer_c;

// What the autotest reads (DummyPlayerDebugInfo::itemFx).
struct DummyItemFxDebug {
    uint8_t drawn[kItemFxKindCount] = {};  // copies drawn this tick, per kind
    uint32_t seen[kItemFxKindCount] = {};  // objects shown, per kind
    uint32_t explosions = 0, hitMarks = 0, sounds = 0, splashes = 0;  // events played
    uint32_t particles = 0;      // particle events played
    uint32_t eventsSkipped = 0;  // events that passed while hidden or across a snap
    uint8_t emitters = 0;        // trail, fuse, tornado and spark emitters alive this tick
    uint8_t lights = 0;          // explosion and rod ball lights on
    bool tornado = false;        // the boomerang's tornado drawn this tick
    bool boomCharge = false;     // the boomerang in hand glows
    uint32_t levelSfxTicks = 0;  // ticks a level sound of the remote was started ("ls")
    uint32_t lastLevelSfx = 0;   // the last one's id
};

class DummyItemFx {
public:
    static constexpr int kArrowPool = 8;
    static constexpr int kBombArrowPool = 3;
    static constexpr int kSeedPool = 4;
    static constexpr int kBombPool = 3;
    static constexpr int kInsectPool = 2;

    // Built by daDummyPlayer_c::createHeapImpl from its private Alink.arc.
    struct Models {
        J3DModel* arrow[kArrowPool] = {};
        J3DModel* bombArrow[kBombArrowPool] = {};
        J3DModel* seed[kSeedPool] = {};
        J3DModel* boomerang = nullptr;
        J3DModel* tornado = nullptr;
        J3DAnmTransform* tornadoBck = nullptr;
        J3DAnmTextureSRTKey* tornadoBtk = nullptr;
        J3DModel* boomGlow = nullptr;
        J3DAnmTextureSRTKey* boomGlowBtk = nullptr;
        J3DModel* bomb[kBombPool] = {};
        J3DModel* waterBomb[kBombPool] = {};
        J3DModel* insectBomb[kInsectPool] = {};
        J3DAnmTransform* insectMoveBck = nullptr;
        J3DAnmTransform* insectWaitBck = nullptr;
        J3DModel* spinner = nullptr;
        J3DAnmTransform* spinnerBck = nullptr;  // SPOUT
        J3DModel* crodBall = nullptr;
        J3DAnmTevRegKey* crodBrk = nullptr;
        J3DAnmTextureSRTKey* crodBtk = nullptr;
        J3DAnmTransform* crodWaitBck = nullptr;    // CROD_BALL_WAIT_A
        J3DAnmTransform* crodAimBck = nullptr;     // CROD_BALL_WAIT_A_T
    };

    // In createHeap
    void createHeap(const Models& models);
    void clearPointers();
    // Once, after the heap exists
    void init();

    // Once per dummy tick, after its matrices
    void update(daDummyPlayer_c& dummy, const LinkPuppetState& state, double shownSeq,
                const ItemFxEventQueue& events, bool snapped, bool shown);
    // Stops everything shown (emitters, sounds, lights) for a tick update does not run.
    void stopAll();
    void draw(daDummyPlayer_c& dummy);
    void destroy();

    const DummyItemFxDebug& debug() const { return mDebug; }

private:
    enum Pool : uint8_t {
        kPoolArrow,
        kPoolBombArrow,
        kPoolSeed,
        kPoolBoomerang,
        kPoolBomb,
        kPoolWaterBomb,
        kPoolInsect,
        kPoolSpinner,
        kPoolCrod,
        kPoolCount,
    };
    static constexpr int kMaxPoolSize = 8;
    static constexpr int kMaxEmitters = 5;
    static constexpr int kExplosions = 2;

    struct Visual {
        ItemFxSlot slot;           // the object shown; kind none for an empty slot
        J3DModel* model = nullptr;
        int8_t pool = -1;
        int8_t poolIndex = -1;
        uint8_t freshTicks = 0;    // not drawn yet: its model's last matrices were elsewhere
        bool placed = false;       // draw() draws it this tick
        uint16_t stateTicks = 0;   // ticks in the slot's current state
        cXyz pos;                  // where it is: its sounds and effects
        cXyz fxPos, fxPosPrev;     // the fuse or smoke point
        cXyz trace;                // the fuse sparks' movement (their particle callback)
        dKy_tevstr_c tevStr;
        uint32_t emitters[kMaxEmitters] = {};
        uint8_t trailAlpha = 0;    // an arrow's trail fading after its flight (decAlphaBlur)
        int16_t spin = 0;          // the boomerang's turn (setRotAngle)
        float frame = 0.0f;        // the tornado's or bombling's clip frame
        int16_t fuse = 0;          // ticks of fuse left
        float scale = 1.0f;
        float groundY = 0.0f;      // a bomb's or spinner's shadow; -inf without ground here
        uint32_t shadowId = 0;     // the spinner's real shadow
        dBgS_ObjGndChk gnd;
        Z2SoundObjSimple sound;
        Z2SoundObjArrow arrowSound;  // daArrow_c's own kind, for the flight sounds
    };

    struct Explosion {
        LIGHT_INFLUENCE light{};
        WIND_INFLUENCE wind{};
        cXyz pos;
        float strength = 0.0f;
        uint8_t mode = 2;  // procExplode's mExplodeMode: 0 flaring, 1 fading, 2 over
        bool lightOn = false;
        bool windOn = false;
    };

    static int tornadoJointCallBack(J3DJoint* joint, int calcTiming);

    Pool poolFor(const ItemFxSlot& s) const;
    void acquire(daDummyPlayer_c& dummy, Visual& v, const ItemFxSlot& s);
    void release(Visual& v);
    void updateArrow(Visual& v, double shownSeq, s8 reverb);
    void updateBoomerang(Visual& v, s8 reverb);
    void updateBomb(Visual& v, double shownSeq, s8 reverb);
    void updateSpinner(Visual& v);
    void updateCrod(daDummyPlayer_c& dummy, Visual& v);
    void cutCrodLight();
    void updateCharge(daDummyPlayer_c& dummy, const LinkPuppetState& state, bool shown);
    void updateLevelSfx(daDummyPlayer_c& dummy, const LinkPuppetState& state, bool shown);
    void fireEvents(double shownSeq, const ItemFxEventQueue& events, bool snapped, bool shown);
    void play(const ItemFxEvent& ev);
    void explode(const cXyz& pos, int16_t rotY, bool underwater);
    void updateExplosions();
    void endExplosion(Explosion& e);
    void drawArrow(Visual& v);
    void drawBomb(Visual& v);
    void drawSpinner(Visual& v);

    J3DModel* mPools[kPoolCount][kMaxPoolSize] = {};
    uint8_t mPoolSize[kPoolCount] = {};
    bool mPoolUsed[kPoolCount][kMaxPoolSize] = {};
    J3DModel* mTornado = nullptr;
    J3DAnmTextureSRTKey* mTornadoBtk = nullptr;
    mDoExt_bckAnm mTornadoBck;
    bool mTornadoBckReady = false;
    int16_t mTornadoAng[3] = {};  // the tornado's shape angle, for its upright joint
    bool mTornadoPlaced = false;
    J3DModel* mBoomGlow = nullptr;
    J3DAnmTextureSRTKey* mBoomGlowBtk = nullptr;
    bool mBoomGlowPlaced = false;
    uint32_t mBoomGlowEmitter = 0;
    cXyz mChargePos;
    Z2SoundObjSimple mChargeSound;
    mDoExt_bckAnm mInsectMoveBck;
    mDoExt_bckAnm mInsectWaitBck;
    bool mInsectBckReady = false;
    mDoExt_bckAnm mSpinnerBck;
    bool mSpinnerBckReady = false;
    J3DAnmTevRegKey* mCrodBrk = nullptr;
    J3DAnmTextureSRTKey* mCrodBtk = nullptr;
    mDoExt_bckAnm mCrodWaitBck;
    mDoExt_bckAnm mCrodAimBck;
    bool mCrodBckReady = false;
    LIGHT_INFLUENCE mCrodLight{};  // daCrod_c's mLight, for the one ball there is
    bool mCrodLightOn = false;

    Visual mVisual[kItemFxSlots];
    Explosion mExplosions[kExplosions];
    dKy_tevstr_c mExplosionTev;
    uint32_t mEventCursor = 0;
    bool mEventCursorValid = false;
    bool mShown = false;
    bool mSoundsReady = false;
    DummyItemFxDebug mDebug;
};

}  // namespace twili

