#pragma once

// A remote player's Epona: a daHorse_c that never runs daHorse_c's create, execute or draw and
// never becomes dComIfGp_getHorseActor(). It reuses daHorse's pure pose members on models from
// a privately mounted Horse.arc, has no attention flags and no collision, so only our own Epona
// can be ridden. Live, its owner's dummy poses it; parked, it stands where the owner's save says.

#include "fx/PlayerRecolor.hpp"
#include "presence/RemotePose.hpp"

#include "d/actor/d_a_horse.h"
#include "d/d_bg_s_gnd_chk.h"

#include <mods/svc/actor.h>

#include <cstdint>
#include <random>

class JKRArchive;
class daAlink_c;

namespace twili {

struct Client;
class daDummyHorse_c;

// What the autotest checks about a puppet.
struct DummyHorseDebugInfo {
    uint32_t clientId = 0;
    bool parked = false;
    bool posed = false;
    bool shown = false;
    bool ridden = false;
    bool demoHeld = false;
    uint16_t anm[3] = {};
    float pos[3] = {};
    float headPos[3] = {};        // joint 15
    float saddlePos[3] = {};      // joint 21
    uint32_t attentionFlags = 0;  // must stay 0
    uint32_t refused = 0;         // clip ids that are not Horse.arc BCKs
    uint32_t heapUsed = 0;
    uint32_t draws = 0;
    int16_t reinPoints = 0;
    bool recolorBound = false;
    uint32_t recolorKey = 0;
    Hsv mane{}, manePristine{};
    uint32_t soundAnims = 0;
};
bool GetDummyHorseDebugInfo(fopAc_ac_c* actor, DummyHorseDebugInfo& out);
// Looked up by process id every call: the manager may delete the puppet between two phases.
daDummyHorse_c* FindDummyHorse(uint32_t clientId);

class daDummyHorse_c : public daHorse_c {
public:
    cPhs_Step create();
    int execute();
    int draw();
    void destroy();
    int createHeap();

    // Live: from the owner's dummy right after its playout advanced, with the sample it shows.
    void applyRemotePose(const RemoteHorsePose& pose, float frameAlpha, bool snapped);
    // Where the rider's origin sits on this horse as posed here.
    bool riderWorldPos(const RemoteHorseRider& rider, cXyz& out);
    // After the rider's calc: feet into the stirrups, reins into its hands.
    void applyRider(daAlink_c& rider, const RemoteHorsePose& pose);
    bool isShown() const;
    bool isRidden() const { return mRidden && isShown(); }
    uint32_t clientId() const { return mClientId; }
    void getDebugInfo(DummyHorseDebugInfo& out) const;

private:
    int createHeapImpl();
    const Client* ownerClient() const;
    bool parkedWanted(const Client& c) const;
    void poseParked(const Client& c);
    bool bindPack(int pack, uint16_t anm, float frame, float frameNext, float alpha);
    void finishPose(bool jump);
    void updateGround();
    void updateSound(bool shown);
    void updateReinsNormal(bool reset);
    void updateReinsHand(daAlink_c& rider, int type, bool reset);
    void copyReinsToLine();

    JKRArchive* mpArchive = nullptr;
    // Every Horse.arc BCK with its BAS, loaded in createHeap before the heap is trimmed.
    mDoExt_transAnmBas* mBck[kHorseLastAnm + 1] = {};
    HorseRecolor mRecolor;
    uint32_t mClientId = 0;
    uint32_t mHeapUsed = 0;
    uint16_t mBoundAnm[3] = {};
    uint8_t mEpoch = 0;
    uint8_t mFlags = 0;  // HorseFlag of the pose shown
    bool mPosed = false;
    bool mParked = false;
    bool mRidden = false;
    bool mDemoHeld = false;
    bool mHidden = false;
    uint32_t mExecTicks = 0;
    uint32_t mLastPushTick = 0;
    uint32_t mRefused = 0;
    uint32_t mDraws = 0;
    uint32_t mSoundAnims = 0;
    int mBlinkFrame = 0;
    std::minstd_rand mBlinkRng;  // never cM_rnd: a remote horse must not move the game's RNG
    dBgS_ObjGndChk mGndChk;
    f32 mGroundY = -G_CM3D_F_INF;
    int8_t mSoundPack = -1;
    uint16_t mSoundAnm = 0;
    float mSoundFrame = 0.0f;
    float mParkedFrame = 0.0f;
};

extern const ActorProfileDesc g_dummyHorseProfile;

}  // namespace twili
