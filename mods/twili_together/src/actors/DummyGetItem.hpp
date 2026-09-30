#pragma once

#include "presence/RemotePose.hpp"

#include "d/d_kankyo_tev_str.h"
#include "SSystem/SComponent/c_phase.h"
#include "SSystem/SComponent/c_xyz.h"

#include <cstdint>

class J3DModel;
class JKRSolidHeap;
class fopAc_ac_c;
class mDoExt_bckAnm;
class mDoExt_brkAnm;
class mDoExt_btkAnm;
class mDoExt_btpAnm;

namespace twili {

// What the autotest reads (DummyPlayerDebugInfo::getItem).
struct DummyGetItemDebug {
    uint8_t wanted = 0xFF;     // display item the pose holds up, 0xFF for none
    uint8_t item = 0xFF;       // display item whose model is built, 0xFF for none
    bool held = false;         // the pose has it drawn there
    bool loading = false;      // its archive is on its way
    bool placed = false;       // drawn this tick
    uint32_t placedTicks = 0;  // ticks drawn
    uint32_t builds = 0;       // models built
    uint32_t failures = 0;     // items that could not be shown
    uint32_t heapUsed = 0;     // of the latest model's heap
    float scale = 0.0f;        // this tick's
    float offset[3] = {};      // where it is drawn: from the dummy, turned by -shape_angle.y
    uint8_t emitters = 0;      // effect emitters alive this tick
};

// daDitem_c's model held up by a remote player, built from the game's item archive.
class DummyGetItem {
public:
    // Each dummy tick after its matrices; `pos` and `yaw` are the dummy's current.pos and
    // shape_angle.y, `shown` false while the dummy is hidden.
    void update(const RemoteGetItem& gi, const cXyz& pos, s16 yaw, bool shown);
    // For a tick update does not run (the remote is elsewhere): frees everything.
    void stop();
    void draw(fopAc_ac_c& dummy);
    // True once no archive load is under way; call until then before destroy.
    bool settled();
    void destroy();

    const DummyGetItemDebug& debug() const { return mDebug; }

private:
    enum class Load : uint8_t { Idle, Loading, Loaded, Failed };

    bool request();
    bool build();
    void release();
    void fail(const char* why);
    void place(const RemoteGetItem& gi, const cXyz& pos, s16 yaw);
    void setEffects(const RemoteGetItem& gi);

    request_of_phase_process_class mPhase{};
    Load mLoad = Load::Idle;
    const char* mArc = nullptr;
    uint8_t mItem = 0xFF;  // display item requested
    bool mBuildTried = false;
    bool mEffectMtx = false;
    s8 mTevFrm = -1;
    s8 mBtpFrm = -1;
    JKRSolidHeap* mHeap = nullptr;
    J3DModel* mModel = nullptr;
    mDoExt_btkAnm* mBtk = nullptr;
    mDoExt_bckAnm* mBck = nullptr;
    mDoExt_brkAnm* mBrk = nullptr;
    mDoExt_btpAnm* mBtp = nullptr;
    bool mPlaced = false;
    cXyz mPos = cXyz(0.0f, 0.0f, 0.0f);
    cXyz mEyePos = cXyz(0.0f, 0.0f, 0.0f);
    dKy_tevstr_c mTevStr;
    bool mTevReady = false;
    uint32_t mEmitters[2] = {};
    DummyGetItemDebug mDebug;
};

}  // namespace twili
