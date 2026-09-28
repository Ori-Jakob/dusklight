#pragma once

// Remote player poses

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace twili {

struct Client;

// PLAYER_UPDATE "md"[0] bits 0-1.
enum MidnaRideMode : uint8_t {
    kMidnaNone = 0,        // not on the back of the sender's wolf, or hidden by its game
    kMidnaDrawn = 1,       // daMidna_c::draw's md.bmd branch
    kMidnaShadowOnly = 2,
};

// "md"[0] bits 4-8.
enum MidnaPoseFlag : uint8_t {
    kMidnaTired = 1 << 0,         // Link binds the tired colour BRKs (d_a_alink.cpp execute)
    kMidnaFaceFromChin = 1 << 1,  // the face layer starts after CHIN, not HEAD (mJntNo)
    kMidnaNoHairScale = 1 << 2,   // FLG0_NO_HAIR_SCALE
    kMidnaHairFromBck = 1 << 3,   // FLG0_UNK_200000 / 10000000: the hair tip does not swing
    kMidnaNoShadow = 1 << 4,      // drawn, but the wolf's real shadow leaves her out (Cloud Sea)
};

// Midna on the sender wolf's back
struct RemoteMidnaPose {
    uint8_t mode = kMidnaNone, flags = 0, hairHand = 0;  // hairHand: 0 tip, 1 hand, 2 pointing
    uint16_t bodyBck = 0, upperBck = 0, faceBck = 0, btp = 0, btk = 0;
    uint8_t leftHand = 0xFE, rightHand = 0xFE;  // md_hands material, 0xFE md.bmd's own hand
    float bodyFrame = 0.0f, upperFrame = 0.0f, faceFrame = 0.0f;
    int16_t neckX = 0, neckY = 0, backboneZ = 0, hairTipY = 0, hairTipZ = 0;
    // Set by RemotePoseBuffer like the body packs' *FramesNext (frameAlpha is shared).
    float bodyFrameNext = 0.0f, upperFrameNext = 0.0f, faceFrameNext = 0.0f;
    // The hair hand turned toward the remote's lock target (setBodyPartMatrix).
    bool hairAimValid = false;
    int16_t hairAim = 0;
};

// PLAYER_UPDATE "wx"[0] bits
enum WolfFxFlag : uint8_t {
    kWolfFxSpin = 1 << 0,         // PROC_WOLF_ROLL_ATTACK: setWolfRollAttackEffect runs
    kWolfFxSpinRight = 1 << 1,    // mProcVar2.field_0x300c: rot.x not turned over
    kWolfFxSpinNoTrail = 1 << 2,  // mProcVar3.field_0x300e: the thaw spin, no KAITENAT_B
    kWolfFxCharge = 1 << 3,       // PROC_WOLF_ROLL_ATTACK_CHARGE / _MOVE (B held)
    kWolfFxDome = 1 << 4,         // Midna's dome is up (mEquipItem 0x109), radius in wx[1]
    kWolfFxLockBlur = 1 << 5,     // player status1 0x01000000: the lock jumps' tail blur
    kWolfFxHairAim = 1 << 6,      // wx[3]: Midna's hair hand turned toward the lock target
    kWolfFxWireMask = 0x7F,
};

// A dome radius past this is junk (the real one stops at 550, daAlinkHIO_wlAtLock_c0).
inline constexpr float kMaxWolfDomeRadius = 1000.0f;

// The sender wolf's attack effects.
struct RemoteWolfFx {
    uint8_t flags = 0;        // WolfFxFlag
    uint8_t lockDashSeq = 0;  // +1 per procWolfLockAttackInit (one LOCKATDASH burst each)
    uint8_t lockCount = 0;    // mWolfLockNum, for tests
    float domeRadius = 0.0f;  // mSearchBallScale while kWolfFxDome
    int16_t hairAim = 0;      // while kWolfFxHairAim
};

// PLAYER_UPDATE "tf"[0] bits
enum TransformFxFlag : uint8_t {
    kTfActive = 1 << 0,    // checkMetamorphose(): PROC_METAMORPHOSE and field_0x300a == 0
    kTfPostSwap = 1 << 1,  // mProcVar5.field_0x3012: the new body is bound
    kTfToWolf = 1 << 2,    // the direction, latched on the proc's first active tick
    kTfWireMask = kTfActive | kTfPostSwap | kTfToWolf,
    kTfAnchorValid = 1 << 7,
};

// The sender's transformation, as its own effects and draw() are made from it.
struct RemoteTransformFx {
    uint8_t flags = 0;
    int16_t tev = 0;          // mProcVar3.field_0x300e, the body's TEV colour (-255..255)
    float hatScale = 1.0f;    // field_0x347c, the X scale of hat joints >= 6
    float anchor[3] = {};     // field_0x37c8, where the emitters sit (world)
    bool active() const { return (flags & kTfActive) != 0; }
    bool postSwap() const { return (flags & kTfPostSwap) != 0; }
    bool toWolf() const { return (flags & kTfToWolf) != 0; }
};

// PLAYER_UPDATE "sx"[0] bits 0-7
enum StatusFlag : uint8_t {
    kStatusFrozen = 1 << 0,    // FLG1_FREEZE_DAMAGE: draw's freeze tint
    kStatusIceBlock = 1 << 1,  // ...and a proc that shows the ice block (setFreezeEffect)
    kStatusElec = 1 << 2,      // PROC_ELEC_DAMAGE (setElecDamageEffect)
    // Magic Armor worn and out of rupees
    kStatusArmorDrained = 1 << 3,
    kStatusWireMask = kStatusFrozen | kStatusIceBlock | kStatusElec | kStatusArmorDrained,
};

// One of the sender's four fire points (daAlink_c::firePointEff_c).
struct RemoteFirePoint {
    uint8_t joint = 0xFF;  // 0xFF: not burning
    uint8_t gen = 0;
    int8_t off[3] = {};    // field_0x18: where on the joint, joint space, whole units
    bool active() const { return joint != 0xFF; }
};

// The sender's status effects.
struct RemoteStatusFx {
    uint8_t flags = 0;             // StatusFlag
    uint8_t shieldBurnOutSeq = 0;  // +1 (mod 8) on every burn-out of the wooden shield
    uint8_t shieldBurn = 0;        // field_0x2fcb, 120 -> 0 while the wooden shield burns
    uint8_t damageTimer = 0;       // mDamageTimer (0 while magic armor takes the hits)
    uint8_t damageColorTime = 0;   // mDamageColorTime
    uint8_t iceWait = 0;           // mIceDamageWaitTimer: the chill before freezing
    float sinkOffset = 0.0f;       // mSinkShapeOffset: < 0 sunk into sand or snow
    RemoteFirePoint fire[4];
};

// PLAYER_UPDATE "x0".."x7"
inline constexpr int kItemFxSlots = 8;

enum ItemFxKind : uint8_t {
    kItemFxNone = 0,
    kItemFxArrow = 1,      // daArrow_c: an arrow, a bomb arrow or a slingshot seed
    kItemFxBoomerang = 2,  // daBoomerang_c thrown, with its tornado
    kItemFxBomb = 3,
    kItemFxSpinner = 4,    // daSpinner_c under the sender
    kItemFxCrodBall = 5,   // daCrod_c: the Dominion Rod's ball, while it glows
    kItemFxKindCount,
};

enum ItemFxArrowSub : uint8_t { kItemFxArrowNormal = 0, kItemFxArrowBomb = 1, kItemFxArrowSling = 4 };
enum ItemFxBombSub : uint8_t { kItemFxBombNormal = 0, kItemFxBombWater = 1, kItemFxBombInsect = 2 };

// Slot states, per kind (the daArrow_c param, daBoomerang_c's return flag, daNbomb_c's proc).
enum ItemFxState : uint8_t {
    kItemFxArrowFly = 1,         // param 1, or 2 for a charged shot
    kItemFxArrowStuckBg = 2,     // param 4
    kItemFxArrowStuckActor = 3,  // param 3
    kItemFxArrowRebound = 4,     // param 5, tumbling away
    kItemFxArrowSlingHit = 5,    // param 8, the seed's burst: not drawn
    kItemFxArrowHeld = 6,        // param 6, an actor took control of it
    kItemFxBoomOut = 1,
    kItemFxBoomReturn = 2,
    kItemFxBombCarried = 1,      // in the sender's hands (or on its clawshot)
    kItemFxBombFree = 2,         // thrown, set down, rolling, walking
    kItemFxBombSinking = 3,      // FLG0_UNK_800: a bomb that went into water, fading out
    kItemFxSpinnerOut = 1,
    kItemFxCrodAtRod = 1,        // param 0/1: on the rod's head (the dummy's own rod places it)
    kItemFxCrodFly = 2,          // param 2/3: thrown
    kItemFxCrodStatue = 3,       // param 4: in a statue it controls
    kItemFxCrodReturn = 4,       // param 5
};

// Only the low 3 bits reach receivers older than the spinner.
enum ItemFxFlag : uint8_t {
    kItemFxArrowCharge = 1 << 0,      // a charged shot (its own flight sound)
    kItemFxArrowFrozen = 1 << 1,      // it went through ice: freeze colour
    kItemFxArrowUnderwater = 1 << 2,
    kItemFxBombUnderwater = 1 << 0,
    kItemFxBombFrozen = 1 << 1,
    kItemFxBombTimerStop = 1 << 2,    // checkTimerStop: no fuse, aux is the scale
    kItemFxSpinnerRail = 1 << 0,      // checkPathMoveNow: grinding a rail
    kItemFxSpinnerSparks = 1 << 1,    // reflectAccept: setSpreadEffect's sparks
    kItemFxSpinnerReverse = 1 << 2,   // spinning the other way: the sparks turn over
    kItemFxSpinnerRidden = 1 << 3,    // under its rider: no shadow of its own
    kItemFxSpinnerTagInto = 1 << 4,   // checkSpinnerTagInto: it dropped into a spinner slot
    kItemFxCrodAim = 1 << 0,          // param 1: the rod is aimed (WAIT_A_T, the aimed hum)
};

struct ItemFxSlot {
    uint8_t kind = kItemFxNone, sub = 0, state = 0, flags = 0;
    uint16_t id = 0;
    float pos[3] = {};
    int16_t ang[3] = {};
    uint16_t aux = 0;
    bool active() const { return kind != kItemFxNone; }
};

// PLAYER_UPDATE "hk"
enum ItemFxHookMode : uint8_t {  // mItemMode (d_a_alink_hook.inc HS_MODE_*; 2 is sent as 1)
    kItemFxHookNone = 0,
    kItemFxHookReady = 1,
    kItemFxHookShoot = 3,
    kItemFxHookFly = 4,     // stuck in a wall or ceiling, pulling the sender
    kItemFxHookCarry = 5,   // stuck in something it drags
    kItemFxHookReturn = 6,
};
enum ItemFxHookSub : uint8_t {  // where the other tip is (double clawshots, hanging)
    kItemFxHookSubHome = 0,
    kItemFxHookSubRoof = 1,    // player status1 0x10000: hanging from a ceiling
    kItemFxHookSubWall = 2,    // 0x2000000: hanging on a wall
    kItemFxHookSubReturn = 3,  // field_0x3024: flying back to the hand
};

struct RemoteHookshot {
    uint8_t mode = kItemFxHookNone;
    uint8_t hand = 0;
    uint8_t sub = kItemFxHookSubHome;
    uint8_t stopTime = 0;   // field_0x3026: the chain's wobble after a catch
    float tip[3] = {};      // mHookshotTopPos while out (mode shoot and later)
    int16_t tipAng[2] = {}; // field_0x301c, field_0x301e
    float tipFrame = 0.0f;  // field_0x33e0: the tip's open clip
    float subTip[3] = {};   // mIronBallBgChkPos while sub != home
    int16_t subAng = 0;     // field_0x3022
    bool out() const { return mode >= kItemFxHookShoot; }
    bool active() const { return mode != kItemFxHookNone || sub != kItemFxHookSubHome; }
};

// PLAYER_UPDATE "bc"
struct RemoteIronBall {
    uint8_t mode = 0;
    bool aim = false;
    int16_t links = 6;      // mItemMode: links out of the hand (getIronBallHandChainNum)
    float ball[3] = {};     // mIronBallChainPos[0] while mode != 0
    int16_t ballAng[3] = {};  // mIronBallChainAngle[0]
    bool active() const { return mode != 0 || aim; }
};

// PLAYER_UPDATE "ls"
inline constexpr int kItemFxLevelSfx = 4;
struct RemoteLevelSfx {
    uint8_t kind = 0;       // PlayerSfxKind (a Level one), 0 for none
    uint8_t mapInfo = 0;    // mPolySound for MapInfoLevel
    uint32_t id = 0;        // Z2 sound id
};

// PLAYER_UPDATE "hx"
struct RemoteHeldExtra {
    uint8_t grassType = 0;   // the grass whistle (0x104): 1 J_Tobi, 2 J_Umak (0: none)
    uint8_t bottleBtk = 0;
    float bottleBtkFrame = 0.0f;
    float bottleBtpFrame = 0.0f;  // field_0x072c: the contents' pattern (milk half gone...)
};

struct RemoteItemFx {
    ItemFxSlot slots[kItemFxSlots];
    RemoteHookshot hk;
    RemoteIronBall bc;
    RemoteLevelSfx ls[kItemFxLevelSfx];
    RemoteHeldExtra hx;
};

// PLAYER_UPDATE "xe"
enum ItemFxEventType : uint8_t {
    kItemFxEvNone = 0,
    kItemFxEvExplode = 1,   // arg 1: under water
    kItemFxEvHitMark = 2,   // arg: dComIfGp_setHitMark type
    kItemFxEvSound = 3,
    kItemFxEvWater = 4,     // arg: splash scale x 16
    kItemFxEvParticle = 5,  // arg: id | (count - 1) << 16 | scale x 16 << 20 (0: 1): ids
    kItemFxEvTypeCount,
};

struct ItemFxEvent {
    uint8_t type = kItemFxEvNone;
    uint32_t arg = 0;
    float pos[3] = {};
    int16_t rot[2] = {};  // x, y
};

class ItemFxEventQueue {
public:
    static constexpr size_t kCapacity = 32;
    struct Entry {
        uint32_t seq = 0;
        ItemFxEvent ev;
    };
    void clear() { mHead = mCount = 0; }
    void push(uint32_t seq, const ItemFxEvent& ev) {
        if (mCount == kCapacity) {
            mHead = (mHead + 1) % kCapacity;
            mCount--;
        }
        mRing[(mHead + mCount) % kCapacity] = {seq, ev};
        mCount++;
    }
    size_t size() const { return mCount; }
    const Entry& at(size_t i) const { return mRing[(mHead + i) % kCapacity]; }

private:
    std::array<Entry, kCapacity> mRing{};
    size_t mHead = 0;
    size_t mCount = 0;
};

struct TransformFxTrace {
    uint32_t startSeq = 0;  // first sample with kTfActive
    uint32_t clipSeq = 0;
    uint32_t swapSeq = 0;   // first active sample with the model swap flag
    uint32_t postSeq = 0;   // first sample with kTfPostSwap
    uint32_t flipSeq = 0;   // first sample in the other form
    uint32_t endSeq = 0;    // first sample without kTfActive after startSeq
    float endFrame = 0.0f;  // the lower clip's frame in the last active sample
    bool toWolf = false;
    uint16_t count = 0;     // transformations seen; a new one resets the rest
};

// PLAYER_UPDATE "ox"[0] bits 0-7 (bits 8-15 carry the horse epoch)
enum HorseFlag : uint8_t {
    kHorsePresent = 1 << 0,      // the sender's daHorse_c is drawn this tick
    kHorseRidden = 1 << 1,       // FLG0_UNK_1 (onRideFlgSubstance)
    kHorseBagHidden = 1 << 2,    // material 5 hidden (RFLG0_UNK_200, or Zelda behind Link)
    kHorseReinsHidden = 1 << 3,  // RFLG0_UNK_100: a demo hides the reins
    kHorseDemo = 1 << 4,
    kHorseReinReset = 1 << 5,    // RFLG0_UNK_1: a cut, no rein continuity
    kHorseWireMask = 0x3F,
};

struct RemoteHorseRider {
    bool active = false;
    uint8_t rootMode = 0;    // field_0x2f99, the root-translation mode of Link's calc
    uint8_t stirrups = 0;    // field_0x2fab & 3: 1 left foot, 2 right foot in its stirrup
    int8_t reinHand = -1;
    bool pitchComp = false;
    float base[3] = {};      // field_0x3588.x, field_0x33b0, field_0x3588.z
    float off[3] = {};       // Link's current.pos in the horse root joint's space
};

inline constexpr uint16_t kHorseFirstAnm = 6;     // Horse.arc BCKs (ANM_HS_BACK_WALK)
inline constexpr uint16_t kHorseLastAnm = 0x23;   // ANM_HS_WALK_SLOW

struct RemoteHorsePose {
    uint8_t flags = 0;       // HorseFlag
    uint8_t epoch = 0;       // bumped on a new horse actor, a reappearance or a jump
    float pos[3] = {};
    int16_t angle[3] = {};   // shape_angle: slope pitch, yaw, roll
    uint16_t anm[3] = {};    // Horse.arc index per pack (body A, body B, neck), 0 for none
    float frame[3] = {};
    float ratio[2] = {};     // m_anmRatio[0/1]
    int16_t neckYaw = 0, lean = 0, tail[3] = {};  // field_0x16f0, field_0x16fa, field_0x16d4
    int16_t foot[4][4] = {};  // m_footData[i].field_0x4: the legs' ground IK
    RemoteHorseRider rider;
    // Set by RemotePoseBuffer like the body packs' *FramesNext (frameAlpha is shared).
    float frameNext[3] = {};
    bool present() const { return (flags & kHorsePresent) != 0; }
};

struct LinkPuppetState {
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    int16_t angleX = 0, angleY = 0, angleZ = 0;
    int16_t shapeAngleX = 0, shapeAngleY = 0, shapeAngleZ = 0;
    int16_t bodyAngleX = 0, bodyAngleY = 0, bodyAngleZ = 0;
    int16_t bodyTwistY = 0;
    uint16_t upperANMs[3] = {};
    uint16_t lowerANMs[3] = {};
    float upperFrames[3] = {};
    float lowerFrames[3] = {};
    float upperRatios[3] = {};
    float lowerRatios[3] = {};
    int16_t waterDropColors[2][4] = {};
    int16_t swordUpColors[2][4] = {};
    uint8_t colorR = 255;
    uint8_t colorG = 255;
    uint8_t colorB = 255;
    uint8_t swordItem = 0xFF;
    uint8_t shieldItem = 0xFF;
    uint8_t clothesItem = 0xFF;
    uint16_t equipItem = 0xFFFF;
    uint8_t upperBlendMode = 0;
    float upperBlendRatio = 0.0f;
    uint8_t cutType = 0;
    uint8_t swordBlurAlpha = 0;
    bool swordBlurActive = false;
    uint8_t leftHandIndex = 0xFE;
    uint8_t rightHandIndex = 0xFE;
    uint8_t leftHandItemOverride = 0xFF;
    uint8_t rightHandItemOverride = 0xFF;
    uint8_t leftHandGripOverride = 0xFF;
    uint8_t rightHandGripOverride = 0xFF;
    uint16_t leftItemJoint = 10;
    uint16_t rightItemJoint = 15;
    uint16_t itemBckId = 0xFFFF;
    float itemBckFrame = 0.0f;
    bool itemAmmoLoaded = false;
    uint16_t itemProjectileSeq = 0;
    uint8_t itemProjectileType = 0;
    bool swordChargeActive = false;
    float swordChargeFrame = 0.0f;
    bool attentionLock = false;
    bool shieldInHand = false;
    // The sender's body (0 human, 1 wolf)
    int8_t transformStatus = 0;
    // The sender's body model is being swapped (kPresenceModelSwap)
    bool modelSwap = false;
    uint16_t visFlags = 0;  // PlayerVisFlag bits
    // Set by RemotePoseBuffer when it blends two samples
    float frameAlpha = 0.0f;
    float lowerFramesNext[3] = {};
    float upperFramesNext[3] = {};
    RemoteMidnaPose midna;
    RemoteTransformFx tf;
    RemoteStatusFx status;
    RemoteWolfFx wolfFx;
    RemoteItemFx itemFx;
    // The sender streams its items ("xv" in its keyframes)
    bool sendsItemFx = false;
    RemoteHorsePose horse;  // its Epona, posed by the dummy on its puppet (DummyHorse.cpp)
};

// The latest received pose of `client` (LinkPuppetState.cpp).
LinkPuppetState makeLinkPuppetState(const Client& client);

// PLAYER_UPDATE "fl" bits.
enum PresenceFlag : uint8_t {
    kPresenceAttentionLock = 1 << 0,
    kPresenceShieldInHand = 1 << 1,
    kPresenceSwordBlur = 1 << 2,
    kPresenceAmmoLoaded = 1 << 3,
    kPresenceSwordCharge = 1 << 4,
    kPresenceWolf = 1 << 5,
    kPresenceInCutscene = 1 << 6,
    // loadModelDVD is swapping the sender's body (transformation or clothes change).
    kPresenceModelSwap = 1 << 7,
};

// PLAYER_UPDATE "vf" bits
enum PlayerVisFlag : uint16_t {
    kVisHeavyBoots = 1 << 0,   // iron boots on (FLG0_EQUIP_HVY_BOOTS)
    kVisZoraMask = 1 << 1,     // Zora helmet down, i.e. under water (checkZoraWearMaskDraw)
    kVisNoDraw = 1 << 2,       // something hides the sender's body (FLG0_PLAYER_NO_DRAW)
    kVisLanternBelt = 1 << 3,  // the lit lantern hangs on the belt (FLG2_UNK_1, not in hand)
    kVisCopyRodLit = 1 << 4,   // the Dominion Rod's head glows (checkCopyRodTopUse)
    // PvP (pvp/)
    kVisGuard = 1 << 5,
    kVisPvpImmune = 1 << 6,
    kVisBoomerangCharge = 1 << 7,
    // The lantern shown (in hand or on the belt) burns (setLight's FLG1_UNK_80)
    kVisLanternLit = 1 << 8,
    kVisLanternSwing = 1 << 9,
};

// Fixed-point scales of the PLAYER_UPDATE fields.
inline constexpr float kPosScale = 8.0f;
inline constexpr float kFrameScale = 16.0f;
inline constexpr float kRatioScale = 1024.0f;
// The hookshot pull peaks at 150 units per tick (d_a_alink_HIO_data.inc)
inline constexpr float kTeleportDistance = 400.0f;

// PLAYER_UPDATE payload in wire units.
struct WirePose {
    int32_t p[3]{}, a[3]{}, sa[3]{}, ba[3]{}, tw = 0;
    int32_t la[3]{}, ua[3]{}, lf[3]{}, uf[3]{}, lr[3]{}, ur[3]{};
    int32_t wd[8]{}, su[8]{}, eq[4]{}, ub[2]{}, ct = 0, bl = 0, hi[6]{}, ij[2]{}, ib[2]{},
        pr[2]{}, cf = 0, vf = 0;
    // Midna on the back (RemoteMidnaPose)
    int32_t md[7]{}, mf[3]{}, ma[5]{};
    // The transformation (RemoteTransformFx)
    int32_t tf[6]{};
    // Status effects (RemoteStatusFx, StatusFx.hpp)
    int32_t sx[5]{}, sf[8]{};
    // The wolf's attack effects (RemoteWolfFx, WolfFx.hpp)
    int32_t wx[4]{};
    // Item slots "x0".."x7" (ItemFxSlot, ItemFx.hpp)
    int32_t x[kItemFxSlots][9]{};
    // The clawshot (RemoteHookshot)
    int32_t hk[11]{}, bc[7]{}, ls[8]{}, hx[4]{};
    int32_t hsx[6]{}, hsp[3]{}, hss[3]{}, hsa[3]{}, hsf[3]{}, hsw[2]{}, hsk[16]{}, hsr[7]{};
};

struct RemotePoseSample {
    uint32_t seq = 0;    // sender's player tick
    uint8_t epoch = 0;
    LinkPuppetState state;
};

// Playout clock of one dummy.
struct RemotePlayout {
    double renderSeq = 0.0;
    // Shown instead of renderSeq while below it
    double holdSeq = 0.0;
    float errSmooth = 0.0f;
    uint32_t lastNewestSeq = 0;
    int ticksSinceNewData = 0;
    uint8_t epoch = 0;
    bool started = false;
    bool havePos = false;
    float lastPos[3] = {};
};

struct RemotePoseEval {
    bool valid = false;    // false: nothing received yet, `out` untouched
    bool snapped = false;  // the pose jumped: reset animation and effect continuity
};

class RemotePoseBuffer {
public:
    static constexpr size_t kCapacity = 64;  // about 2 s at 30 Hz

    // The jitter estimate survives
    void clear() { mHead = mCount = 0; }
    // `arrivalSec` is only used to estimate jitter
    void push(const RemotePoseSample& sample, double arrivalSec);
    size_t size() const { return mCount; }
    // How far behind the newest sample playout aims to run, in ticks.
    float targetDelayTicks() const;
    // The pose at sender tick `renderSeq`
    bool sampleAt(double renderSeq, LinkPuppetState& out, const RemotePoseSample** shown) const;
    // Moves `playout` on by one local tick and writes the pose to show.
    RemotePoseEval advance(RemotePlayout& playout, LinkPuppetState& out) const;

private:
    const RemotePoseSample& at(size_t i) const { return mRing[(mHead + i) % kCapacity]; }

    std::array<RemotePoseSample, kCapacity> mRing{};
    size_t mHead = 0;
    size_t mCount = 0;
    double mLastArrivalSec = 0.0;
    uint32_t mLastArrivalSeq = 0;
    float mJitter = 0.0f;
};

// Checks the buffer's playout rules against synthetic streams (autotest op poseSelfTest).
bool runRemotePoseSelfTest(std::string& why);

}  // namespace twili
