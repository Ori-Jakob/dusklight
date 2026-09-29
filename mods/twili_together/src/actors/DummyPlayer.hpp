#pragma once

#include "actors/DummyItemFx.hpp"
#include "actors/DummyMidna.hpp"
#include "actors/PrivateArchives.hpp"
#include "fx/PlayerRecolor.hpp"
#include "fx/RemoteTransformFx.hpp"

#include "d/actor/d_a_alink.h"

#include <mods/svc/actor.h>

class JKRArchive;

namespace twili {

struct Client;

// How a dummy replays its remote's transformations (updateRemoteTransformFx), for the autotest.
struct DummyTransformFxTrace {
    uint8_t flags = 0;         // RemoteTransformFx flags shown this tick
    uint8_t emitted = 0;       // emitters set this tick
    bool silhouette = false;   // wl_change.bmd instead of the body this tick
    int16_t bodyTev = 0;
    float hatScale = 1.0f;     // the hat's X scale, 1 when it is not overridden
    double startShown = 0.0, swapShown = 0.0, postShown = 0.0;
    double endShown = 0.0;     // the first tick the transformation no longer showed
    double clipShown = 0.0;
    uint8_t maskA = 0, maskSwap = 0, maskC = 0;  // emitters set before, in and after the swap
    uint16_t ticksA = 0, silhouetteTicks = 0, ticksC = 0, hiddenTicks = 0;
    int16_t tevMinA = 0, tevMinC = 0, tevMaxC = 0;
    bool furA = false, furC = false;  // the fur (item 0x106) was on the body before / after
    float anchorErr = -1.0f;
    uint8_t anchorSource = 0;
    uint16_t count = 0;  // transformations replayed
};

// One PLAYER_SFX a dummy played on its playout clock (autotest).
struct DummySfxPlayed {
    uint32_t id = 0;
    uint32_t seq = 0;     // the sender tick it was stamped with
    double shown = 0.0;   // the sender tick shown when it played
    uint8_t kind = 0;
};

// What the autotest checks about a dummy (GetDummyPlayerDebugInfo).
struct DummyPlayerDebugInfo {
    bool shellReady = false;
    bool remoteWolf = false;    // the pose shown comes from a wolf
    bool wolfBody = false;      // the wolf body is bound (checkWolf)
    bool hidden = false;        // isHidden()
    uint16_t bodyJointNum = 0;  // 35 human, 40 wolf
    uint32_t refusedAnms = 0;   // clip requests refused because they did not fit the body
    // Clips the remote asked for that could not be loaded here, each counted once per pack.
    uint32_t missingAnms = 0;
    bool basePack = false;          // lower pack 0, which the other packs blend onto, holds a clip
    uint16_t basePackAnm = 0xFFFF;  // and this is its id
    bool standIn = false;           // a held or idle clip, for one that could not be loaded
    uint8_t clothes = 0xFF;         // clothes set bound (dItemNo_WEAR_CASUAL_e ...)
    uint8_t colorR = 0, colorG = 0, colorB = 0;  // player colour the clothes are recoloured in
    bool recolorBound = false;      // the worn set can be recoloured (PlayerRecolor)
    int8_t recolorSet = -1;         // RecolorSetId worn
    uint32_t recolorKey = 0;        // key its textures hold (RecolorSet::kPristine: as on disc)
    bool armorDrained = false;      // Magic Armor worn with its power_down BRK bound
    float armorBrkFrame = 0.0f;     // the Magic Armor BRK's frame
    bool armorBrkSettled = false;   // ...played to its last frame, where it holds
    bool casualHead = false;        // FLG2_UNK_100000: the hat model is the 6-joint casual head
    bool heavyBoots = false;
    bool zoraMask = false;          // Zora helmet down
    bool lantern = false;           // lantern drawn, in hand or on the belt
    uint16_t heldItem = 0xFFFF;     // item whose model the hand holds
    bool ground = false;            // ground found under the dummy, so it casts a shadow
    // The sender tick shown; advances about 1 per local tick when smooth.
    double shownSeq = 0.0;
    // Midna on the wolf's back (DummyMidna).
    bool midnaReady = false;       // her models and idle pose loaded
    uint8_t midnaMode = 0;         // MidnaRideMode posed now
    bool midnaShown = false;       // drawn: posed kMidnaDrawn on a shown wolf body
    uint16_t midnaBodyAnm = 0;     // body clip bound
    uint16_t midnaUpperAnm = 0;    // upper layer shown, 0 for none
    uint8_t midnaHairHand = 0xFF;  // hair hand material shown (0 tip, 1 hand, 2 pointing)
    uint8_t midnaLeftHand = 0xFE;
    uint8_t midnaRightHand = 0xFE;
    bool midnaTired = false;       // tired colours bound
    uint32_t midnaRefused = 0;     // Midna clip and texture ids refused
    float midnaBackDist = 0.0f;    // her root from the wolf's joint WL_JNT_MD
    uint32_t midnaApartTicks = 0;  // ticks posed off the wolf's back (kMidnaApart)
    uint32_t midnaShownTicks = 0;  // ticks she was drawn, on the back or off it
    bool midnaEyeMove = false;     // her eyes follow the sender's offsets this tick
    // PvP (pvp/)
    bool hurtbox = false;
    bool hurtboxGuard = false;
    DummyTransformFxTrace tf;
    uint8_t tfAlive = 0;  // bit i: the emitter in transformation slot i still exists
    // Status effects (updateRemoteStatus)
    uint8_t statusFlags = 0;      // StatusFlag bits of the pose shown
    bool frozen = false;          // FLG1_FREEZE_DAMAGE set: draw's freeze tint
    bool iceBlock = false;        // the ice block emitter exists
    uint32_t thaws = 0;           // shatter bursts played
    uint8_t firePoints = 0;       // fire slots lit
    uint8_t fireEmitters = 0;     // lit slots whose flame emitter exists
    uint8_t fireReceived = 0;     // burning slots in the pose shown
    uint8_t shieldBurn = 0;       // field_0x2fcb: draw chars the shield from it
    bool shieldBurnFx = false;    // the shield's flame emitter exists
    uint32_t shieldBurnOuts = 0;  // burn-out bursts played
    uint8_t shieldItem = 0xFF;    // shield bound
    bool elec = false;            // sparks set this tick
    bool elecFx = false;          // the first spark emitter exists
    uint8_t damageTimer = 0;      // mDamageTimer: draw's damage flash
    uint32_t flashes = 0;         // damage flashes started
    uint8_t iceWait = 0;          // mIceDamageWaitTimer: draw's chill
    float sinkOffset = 0.0f;      // mSinkShapeOffset applied to the body matrix
    // Wolf attack effects (DummyWolfFx.cpp).
    uint8_t wolfSpin = 0;          // 0 none, 1 left, 2 right: the spin effect was set this tick
    uint32_t wolfSpinTicks = 0;    // ticks the spin effect was set
    uint8_t wolfLastSpin = 0;      // 1 left, 2 right: the last spin shown
    uint8_t wolfSpinEmitters = 0;  // alive after its last tick's set (2, 1 without the trail)
    bool wolfDome = false;         // Midna's dome model is held
    bool wolfDomeShown = false;    // and drawn (isBodyShown)
    float wolfDomeRadius = 0.0f;   // mSearchBallScale it is scaled from
    uint8_t wolfLockBlurAlpha = 0; // the tail blur's alpha this tick
    uint32_t wolfLockDashes = 0;   // LOCKATDASH bursts made
    bool midnaHairAim = false;     // Midna's hair hand turned toward the remote's lock target
    int16_t midnaHairAimAngle = 0;
    // The remote's weapons and items out in the world (DummyItemFx).
    DummyItemFxDebug itemFx;
    uint32_t heapUsed = 0;  // of the solid heap, at the end of createHeap
    // Held items (updateRemoteHeldItemMatrix)
    bool hookChain = false;
    float hookTipDist = 0.0f;
    uint32_t hookShots = 0;  // chains drawn, counted when one appears
    float hookPeak = 0.0f;   // the longest tip distance of the latest chain
    uint8_t ironBallMode = 0;
    float ironBallDist = 0.0f;
    bool lanternFlame = false;
    float lanternGlow = 0.0f;
    // PLAYER_SFX played on the playout clock (playQueuedSfx)
    uint32_t sfxPlayed = 0;
    uint32_t sfxDropped = 0;  // late, too far ahead, or across a snap
    uint32_t midnaSfx = 0;    // Midna's sounds played (from the wire and her clips)
    DummySfxPlayed sfxRecent[16];  // the latest first
};

// A remote player: a daAlink_c posed from the received stream, never controlled.
class daDummyPlayer_c : public daAlink_c {
public:
    cPhs_Step create();
    int execute();
    int draw();
    void destroy();
    int createHeap();
    void playRemotePlayerSfx(uint32_t soundId, uint8_t kind, uint32_t mapInfo);
    // Link's or Midna's, by kind
    void playRemoteSfx(uint32_t soundId, uint8_t kind, uint32_t mapInfo);
    // Not drawn, heard or given effects
    bool isHidden() const;
    bool isBodyShown() const;
    void getDebugInfo(twili::DummyPlayerDebugInfo& out) const;
    void getRecolorProbe(twili::RecolorProbe& out) const;
    void onPvpTgHit(dCcD_GObjInf* tg, fopAc_ac_c* atActor, dCcD_GObjInf* at);
    void playPvpHitSe(uint32_t hitSe, uint32_t guardSe, bool blocked);

    bool privateArchivesBusy() const { return mDummyArchives.busy(); }

private:
    cPhs_Step loadPrivateArchives();
    int createHeapImpl();
    void clearPrivateModelPointers();
    void unmountPrivateArchives();
    void destroyDummyAnmHeaps();

    int initializeShell(const twili::LinkPuppetState& state);
    void updateRemoteHidden(const twili::Client& client,
                            const twili::LinkPuppetState& state);
    void resetRemoteContinuity();
    void setRemoteBodyMetrics(bool wolf);
    void applyRemoteForm(bool wolf, uint8_t clothesItem);
    void updateRemoteTransformFx(const twili::Client& client,
                                 const twili::LinkPuppetState& state, double shownSeq);
    void traceRemoteTransformFx(const twili::LinkPuppetState& state, uint8_t emitted,
                                double shownSeq, bool wasActive);
    bool setRemoteMetamorphoseModel(uint16_t bckId);
    void updateRemoteStatus(const twili::LinkPuppetState& state);
    void updateRemoteFreezeEffect(const twili::RemoteStatusFx& s, bool shown);
    void updateRemoteFirePoints(const twili::RemoteStatusFx& s, bool shown);
    void releaseRemoteFirePoint(int slot);
    void updateRemoteShieldBurn(const twili::RemoteStatusFx& s, bool shown);
    void clearRemoteStatus();
    bool applyRemoteState(const twili::LinkPuppetState& state);
    void initRemoteHurtbox();
    void updateRemoteHurtbox(const twili::Client& client,
                             const twili::LinkPuppetState& state);
    u16 bodyJointNum() const;
    bool bckFitsBody(J3DAnmTransform* bck) const;
    void dropRemoteAnimationPack(bool upper, int idx);
    J3DAnmTransform* showBasePackFallback(bool heapIntact);
    void forgetRemoteAnimationPacks();
    bool loadRemoteAnimationPacks(const twili::LinkPuppetState& state,
                                  float lowerRatios[3], float upperRatios[3]);
    bool setRemoteAnimations(const twili::LinkPuppetState& state);
    J3DAnmTransform* loadRemoteAnimationPack(bool upper, int idx, uint16_t anmId, float frame,
                                             float frameNext = 0.0f, float frameAlpha = 0.0f);
    bool setRemoteBasAnime(bool upper, int idx);
    void modelCalcRemoteBody(bool attentionLock);
    void applyRemoteClothes(uint8_t clothesItem);
    void bindRemoteMagicArmorBrk(int status, bool fromStart = false);
    void updateRemoteMagicArmor(const twili::LinkPuppetState& state);
    void bindRemoteRecolor();
    twili::RecolorSetId wornRecolorSet() const;
    void updateRemoteRecolor();
    void updateRemoteEquipment(const twili::LinkPuppetState& state);
    void updateRemoteBodyShapes(const twili::LinkPuppetState& state);
    void updateRemoteFace();
    void updateRemoteBootMatrices();
    void updateRemoteLantern(const twili::LinkPuppetState& state);
    void updateRemoteGround();
    void drawRemoteShadow();
    void applyRemoteItemPresentation(const twili::LinkPuppetState& state);
    void updateRemoteLoadedAmmo(const twili::LinkPuppetState& state);
    J3DModel* remoteAmmoModelForType(uint8_t type) const;
    void updateRemoteItemMatrices(const twili::LinkPuppetState& state);
    void clearRemoteHeldItemModel();
    void syncRemoteHeldItemModel(uint16_t equipItem, uint16_t itemBckId);
    bool loadRemoteGrassWhistle();
    void updateRemoteHeldItemMatrix(const twili::LinkPuppetState& state);
    void updateRemoteHookshot(const twili::RemoteHookshot& hk);
    void updateRemoteIronBall(const twili::RemoteIronBall& bc);
    void applyRemoteBottle(const twili::RemoteHeldExtra& hx);
    void updateRemoteLanternFlame(const twili::LinkPuppetState& state, bool lit);
    void updateRemoteSwordEffects(const twili::LinkPuppetState& state);
    bool loadRemoteLockDome();
    void updateRemoteWolfFx(const twili::LinkPuppetState& state);
    void updateRemoteWolfLockBlur(bool active);
    void clearRemoteWolfFx();
    void updateRemoteAudio(const twili::LinkPuppetState& state);
    void playQueuedSfx(const twili::Client& client, double shownSeq, bool snapped);
    void drawRemoteLoadedAmmo();

    JKRArchive* mpDummyKmdlArchive = nullptr;
    JKRArchive* mpDummyMmdlArchive = nullptr;
    JKRArchive* mpDummyZmdlArchive = nullptr;
    JKRArchive* mpDummyAlinkArchive = nullptr;
    JKRArchive* mpDummyHylianShieldArchive = nullptr;
    JKRArchive* mpDummyOrdonShieldArchive = nullptr;
    JKRArchive* mpDummyWoodShieldArchive = nullptr;
    JKRArchive* mpDummyWmdlArchive = nullptr;
    JKRArchive* mpDummyBmdlArchive = nullptr;
    J3DModel* mpDummyKokiriLinkModel = nullptr;
    J3DModel* mpDummyKokiriFaceModel = nullptr;
    J3DModel* mpDummyKokiriHatModel = nullptr;
    J3DModel* mpDummyKokiriHandModel = nullptr;
    J3DModel* mpDummyKokiriWoodSwordModel = nullptr;
    J3DModel* mpDummyZoraLinkModel = nullptr;
    J3DModel* mpDummyZoraFaceModel = nullptr;
    J3DModel* mpDummyZoraHatModel = nullptr;
    J3DModel* mpDummyZoraHandModel = nullptr;
    J3DModel* mpDummyZoraWoodSwordModel = nullptr;
    J3DModel* mpDummyMagicLinkModel = nullptr;
    J3DModel* mpDummyMagicFaceModel = nullptr;
    J3DModel* mpDummyMagicHatModel = nullptr;
    J3DModel* mpDummyMagicHandModel = nullptr;
    J3DModel* mpDummyMagicWoodSwordModel = nullptr;
    J3DModel* mpDummyCasualLinkModel = nullptr;
    J3DModel* mpDummyCasualFaceModel = nullptr;
    J3DModel* mpDummyCasualHatModel = nullptr;
    J3DModel* mpDummyCasualHandModel = nullptr;
    J3DModel* mpDummyCasualWoodSwordModel = nullptr;
    // Iron boots of each clothes set, as changeLink loads them from the set's archive.
    J3DModel* mpDummyKokiriBootModels[2] = {};
    J3DModel* mpDummyZoraBootModels[2] = {};
    J3DModel* mpDummyMagicBootModels[2] = {};
    J3DModel* mpDummyCasualBootModels[2] = {};
    J3DModel* mpDummyBoomerangModel = nullptr;
    J3DAnmTevRegKey* mpDummyMagicArmorBodyBrk[3] = {};
    J3DAnmTevRegKey* mpDummyMagicArmorHeadBrk[3] = {};
    J3DModel* mpDummyHylianShieldModel = nullptr;
    J3DModel* mpDummyOrdonShieldModel = nullptr;
    J3DModel* mpDummyWoodShieldModel = nullptr;
    J3DModel* mpDummyArrowAmmoModel = nullptr;
    J3DModel* mpDummyBombArrowAmmoModel = nullptr;
    J3DModel* mpDummySlingAmmoModel = nullptr;
    J3DModel* mpDummyWolfModel = nullptr;
    J3DModel* mpDummyWolfChainModels[4] = {};
    twili::DummyMidna mDummyMidna;
    twili::DummyItemFx mDummyItemFx;
    uint32_t mDummyHeapUsed = 0;
    uint32_t mDummyClientId = 0;
    twili::RemotePlayout mDummyPlayout;
    uint16_t mDummyLowerActiveAnm[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    uint16_t mDummyUpperActiveAnm[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    uint16_t mDummyLowerLastMissingAnm[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    uint16_t mDummyUpperLastMissingAnm[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    // The last clip each pack refused for not fitting the body, so it is not read again.
    uint16_t mDummyLowerRefusedAnm[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    uint16_t mDummyUpperRefusedAnm[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    uint32_t mDummyRefusedAnmCount = 0;
    uint32_t mDummyMissingAnmCount = 0;
    // Ticks lower pack 0 has kept playing its last clip in place of one it could not load.
    uint8_t mDummyBaseHoldTicks = 0;
    // Lower pack 0 shows a stand-in for the remote's clip (showBasePackFallback).
    bool mDummyBasePackFallback = false;
    uint16_t mDummyLowerDominantAnm = 0xFFFF;
    uint16_t mDummyUpperDominantAnm = 0xFFFF;
    int mDummyLowerDominantPack = -1;
    int mDummyUpperDominantPack = -1;
    float mDummyLowerPrevFrame[3] = {};
    float mDummyUpperPrevFrame[3] = {};
    bool mDummyLowerPrevFrameValid[3] = {};
    bool mDummyUpperPrevFrameValid[3] = {};
    uint8_t mDummyClothesItem = 0xFF;
    J3DModel* mDummyHatPrimed = nullptr;  // hat model setHatAngle has a calc to read from
    uint8_t mDummySwordItem = 0xFF;
    uint8_t mDummyShieldItem = 0xFF;
    uint16_t mDummyHeldItem = 0xFFFF;
    uint8_t mDummyGrassType = 0;      // RemoteHeldExtra::grassType of the pose shown
    uint8_t mDummyHeldGrassType = 0;  // ...and of the grass whistle held
    uint8_t mDummyBottleBtk = 0;      // the bottle clip entered last (RemoteHeldExtra::bottleBtk)
    // Autotest (getDebugInfo)
    bool mDummyHookChain = false;
    float mDummyHookTipDist = 0.0f;
    bool mDummyHookWasChain = false;
    uint32_t mDummyHookShots = 0;
    float mDummyHookPeak = 0.0f;
    uint8_t mDummyHookSub = 0;  // RemoteHookshot::sub shown (for the mode log)
    uint8_t mDummyIronBallMode = 0;
    float mDummyIronBallDist = 0.0f;
    bool mDummyLanternFlame = false;
    uint16_t mDummyVisFlags = 0;  // PlayerVisFlag bits shown
    int mDummyBlinkFrame = 0;     // frame of the idle face's blink, 0 while the eyes are open
    bool mDummyFaceBckReady = false;  // mFaceBck holds the idle face pose (initializeShell)
    // Height of the ground under the dummy, -G_CM3D_F_INF when there is none here.
    f32 mDummyGroundY = -G_CM3D_F_INF;
    J3DModel* mpDummyLoadedAmmoModel = nullptr;
    twili::RemoteHorseRider mDummyRider;
    uint8_t mDummyColorR = 255;
    uint8_t mDummyColorG = 255;
    uint8_t mDummyColorB = 255;
    // The clothes and wolf textures recoloured in that colour, one set each (PlayerRecolor).
    twili::RecolorSet mDummyRecolor[static_cast<size_t>(twili::RecolorSetId::Count)];
    int32_t mDummyRecolorTick = 0;  // applyRemoteState calls, for the rewrite rate limit
    int8_t mDummyRecolorWorn = -1;  // RecolorSetId worn at the last call
    uint32_t mDummyDrawCount = 0;   // autotest: draws that submitted the body
    // The Magic Armor BRK bound (0 power_down, 1 power_up_a, -1 none).
    int8_t mDummyArmorBrkStatus = -1;
    bool mDummyArmorWorn = false;
    // The remote's transformation (updateRemoteTransformFx)
    twili::RemoteTransformFx mDummyTf;
    twili::TransformFxPlan mDummyTfPlan;
    uint32_t mDummyTfEmitter[2] = {};  // the remote's slots field_0x31f8 and field_0x31fc
    cXyz mDummyTfAnchor = cXyz::Zero;  // its field_0x37c8, as this dummy's body had it
    Mtx mDummyTfJointMtx;              // joint 2 on the last tick the old body moved
    bool mDummyTfAnchorValid = false;
    bool mDummyTfSilhouette = false;   // wl_change.bmd is placed for this tick's draw
    uint16_t mDummyTfClipAnm = 0xFFFF; // base clip the last trace tick saw
    twili::DummyTransformFxTrace mDummyTfTrace;
    // The remote's status effects (updateRemoteStatus).
    twili::RemoteStatusFx mDummyStatus;  // of the pose shown
    uint8_t mDummyFireGen[4] = {};              // the generation each slot was lit for
    bool mDummyFireSpent[4] = {};               // our flames ran out: dark until the next one
    uint8_t mDummyShieldBurnOutSeq = 0;
    Mtx mDummyShieldBurnMtx;                    // the shield on the last tick it burned
    bool mDummyShieldBurnMtxValid = false;
    uint32_t mDummyShieldBurnOuts = 0;          // autotest
    uint32_t mDummyThaws = 0;                   // autotest
    uint32_t mDummyFlashes = 0;                 // autotest
    bool mDummyWasFrozen = false;               // the shatter is owed when the freeze ends
    bool mDummyStatusPrimed = false;            // the one-shot counters took the first pose's
    bool mDummyElecShown = false;
    bool mDummyShellReady = false;
    // The boots type mZ2Link last played, -1 for none yet.
    int8_t mDummyBootsSoundType = -1;
    bool mDummyAnimationBlendInitialized = false;
    bool mDummyDrawLogged = false;
    bool mDummySwordBlurWasActive = false;
    bool mDummyCutTurnEffectWasActive = false;
    bool mDummyHidden = false;
    bool mDummyWasWolf = false;
    // A refused clip was read into the buffer all body packs share (initializeShell).
    bool mDummyAnmBufferClobbered = false;
    bool mDummyHurtboxLive = false;
    bool mDummyHurtboxGuard = false;
    // Wolf attack effects (DummyWolfFx.cpp).
    bool mDummyWolfSpinWasActive = false;
    bool mDummyLockDashSeqValid = false;
    uint8_t mDummyLockDashSeq = 0;
    uint8_t mDummyLockBlurAlpha = 0;
    uint8_t mDummyWolfLastSpin = 0;      // autotest: 1 left, 2 right
    uint8_t mDummyWolfSpinEmitters = 0;  // autotest
    uint32_t mDummyWolfSpinTicks = 0;    // autotest
    uint32_t mDummyLockDashes = 0;       // autotest
    // Zero until create() constructed the actor; a force-delete may come before.
    bool mDummyConstructed = true;
    twili::PrivateArchives mDummyArchives;
    bool mDummyArchivesUsed = false;
    // The last queued PLAYER_SFX handled (RemoteSfx::index)
    uint32_t mDummySfxDone = 0;
    bool mDummySfxPrimed = false;
    uint32_t mDummySfxPlayed = 0;
    uint32_t mDummySfxDropped = 0;
    twili::DummySfxPlayed mDummySfxRecent[16];
};

// ActorService profile "TTlink".
extern const ActorProfileDesc g_dummyPlayerProfile;

// Points Z2CreatureLink::mLinkPtr and the audience mic back at the local Link.
void restoreLocalLinkAudioPtr();

// True when `actor` is a dummy player that is currently hidden (autotest).
bool IsDummyPlayerHidden(fopAc_ac_c* actor);
// Whether draw() shows the dummy's body
bool IsDummyPlayerShown(fopAc_ac_c* actor);
// False when `actor` is not a dummy player (autotest).
bool GetDummyPlayerDebugInfo(fopAc_ac_c* actor, DummyPlayerDebugInfo& out);
// The worn set's recolour state (autotest).
bool GetDummyPlayerRecolorProbe(fopAc_ac_c* actor, RecolorProbe& out);
// PvP: the sound of our hit on `actor`, a dummy player.
void PlayDummyPlayerHitSe(fopAc_ac_c* actor, uint32_t hitSe, uint32_t guardSe, bool blocked);
}  // namespace twili

