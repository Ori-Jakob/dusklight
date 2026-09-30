#include "fx/GetItem.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_demo_item.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "SSystem/SComponent/c_math.h"
#include "SSystem/SComponent/c_tag.h"

#include <algorithm>
#include <cmath>

namespace twili::getitem {
namespace {

constexpr float kMaxScale = 4.0f;

struct LinkPlace {
    uint32_t seq = 0;
    cXyz pos = cXyz(0.0f, 0.0f, 0.0f);
    s16 yaw = 0;
    bool valid = false;
};

LinkPlace sPrev;
SenderStats sStats;
bool sWasShown = false;

// procCoGetItem and friends show the event's item partner; dead, its next execute takes it out
// of the draw queue before any draw.
daDitem_c* heldItem(daAlink_c* link) {
    fopAc_ac_c* partner = fopAcM_getItemEventPartner(link);
    if (partner == nullptr || fopAcM_GetName(partner) != fpcNm_Demo_Item_e ||
        fpcM_IsCreating(fopAcM_GetID(partner)))
    {
        return nullptr;
    }
    auto* item = static_cast<daDitem_c*>(partner);
    return item->mpModel != nullptr && !item->chkDead() ? item : nullptr;
}

int32_t wire(float v, float scale) {
    return std::isfinite(v) ? static_cast<int32_t>(std::lround(v * scale)) : 0;
}

}  // namespace

void captureLocal(daAlink_c* link, uint32_t seq, RemoteGetItem& out) {
    out = RemoteGetItem{};
    if (link == nullptr) {
        sPrev.valid = false;
        sWasShown = false;
        return;
    }
    // The item executes after Link, so it stands where last tick's Link placed it.
    const bool prevTick = sPrev.valid && sPrev.seq + 1 == seq;
    const cXyz base = prevTick ? sPrev.pos : link->current.pos;
    const s16 baseYaw = prevTick ? sPrev.yaw : link->shape_angle.y;
    sPrev.seq = seq;
    sPrev.pos = link->current.pos;
    sPrev.yaw = link->shape_angle.y;
    sPrev.valid = true;

    daDitem_c* item = heldItem(link);
    if (item == nullptr) {
        sWasShown = false;
        return;
    }
    // Peers load its model while it is still hidden.
    out.flags = kGetItemPending;
    if (item->chkFlag(4)) {
        out.flags |= kGetItemEffectMtx;
    }
    if (item->chkArgFlag(0x2) || item->chkArgFlag(0x4)) {
        out.flags |= kGetItemNoLight;
    }
    out.displayItem = item->getDisplayItemNo();
    out.realItem = item->getItemNo();
    const float scale = item->mpModel->getBaseScale()->x;
    const cXyz d = item->current.pos - base;
    if (!cTg_IsUse(&item->draw_tag) || !(scale > 0.0f) || !std::isfinite(scale) ||
        d.abs2() > kMaxGetItemOffset * kMaxGetItemOffset)
    {
        sWasShown = false;
        return;
    }
    const f32 s = cM_ssin(baseYaw);
    const f32 c = cM_scos(baseYaw);
    out.offset[0] = c * d.x - s * d.z;
    out.offset[1] = d.y;
    out.offset[2] = s * d.x + c * d.z;
    out.flags = static_cast<uint8_t>((out.flags & ~kGetItemPending) | kGetItemShown);
    out.yaw = static_cast<int16_t>(item->current.angle.y - baseYaw);
    out.scale = std::min(scale, kMaxScale);
    out.timer = static_cast<float>(std::max(item->m_timer, 0));

    sStats.shownTicks++;
    if (!sWasShown) {
        sStats.shows++;
    }
    sStats.lastItem = out.displayItem;
    sStats.lastRealItem = out.realItem;
    sWasShown = true;
}

void encode(const RemoteGetItem& g, int32_t out[kWireSize]) {
    std::fill(out, out + kWireSize, 0);
    if (!g.present()) {
        return;
    }
    out[0] = g.displayItem | (g.realItem << 8) | ((g.flags & kGetItemWireMask) << 16);
    if (!g.shown()) {
        return;
    }
    out[1] = g.yaw;
    out[2] = wire(g.scale, kRatioScale);
    out[3] = wire(g.timer, 1.0f);
    for (int i = 0; i < 3; i++) {
        out[4 + i] = wire(g.offset[i], kPosScale);
    }
}

RemoteGetItem decode(const int32_t in[kWireSize]) {
    RemoteGetItem g;
    const uint32_t head = static_cast<uint32_t>(in[0]);
    g.flags = static_cast<uint8_t>((head >> 16) & kGetItemWireMask);
    g.displayItem = static_cast<uint8_t>(head & 0xFF);
    g.realItem = static_cast<uint8_t>((head >> 8) & 0xFF);
    // No item table entry 0xFF (dItemNo_NONE_e)
    if (!g.present() || g.displayItem == 0xFF) {
        return RemoteGetItem{};
    }
    const uint8_t pending = static_cast<uint8_t>((g.flags & ~kGetItemShown) | kGetItemPending);
    if (!g.shown()) {
        g.flags = pending;
        return g;
    }
    g.flags = static_cast<uint8_t>(g.flags & ~kGetItemPending);
    g.yaw = static_cast<int16_t>(in[1]);
    g.scale = std::clamp(in[2] / kRatioScale, 0.0f, kMaxScale);
    g.timer = static_cast<float>(std::max(in[3], 0));
    float distSq = 0.0f;
    for (int i = 0; i < 3; i++) {
        g.offset[i] = in[4 + i] / kPosScale;
        distSq += g.offset[i] * g.offset[i];
    }
    if (!(distSq <= kMaxGetItemOffset * kMaxGetItemOffset)) {
        RemoteGetItem hidden;
        hidden.flags = pending;
        hidden.displayItem = g.displayItem;
        hidden.realItem = g.realItem;
        return hidden;
    }
    return g;
}

const SenderStats& senderStats() {
    return sStats;
}

}  // namespace twili::getitem
