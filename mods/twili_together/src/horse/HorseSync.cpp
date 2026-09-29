#include "horse/HorseSync.hpp"

#include "core/Client.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_horse.h"
#include "d/actor/d_a_hozelda.h"
#include "d/d_com_inf_game.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "m_Do/m_Do_mtx.h"

#include <dolphin/dvd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace twili::horse {
namespace {

constexpr float kMaxRiderOffset = 500.0f;
constexpr float kRiderBaseScale = 16.0f;
constexpr float kRiderOffsetScale = 4.0f;

struct Sender {
    fpc_ProcID pid = fpcM_ERROR_PROCESS_ID_e;
    bool present = false;
    uint8_t epoch = 0;
    float lastPos[3] = {};
};

Sender s_tx;

int32_t quantize(float value, float scale) {
    if (!std::isfinite(value)) {
        return 0;
    }
    return static_cast<int32_t>(std::clamp<long long>(
        std::llround(static_cast<double>(value) * scale), -2000000000LL, 2000000000LL));
}

bool validAnm(int32_t id) {
    return id >= kHorseFirstAnm && id <= kHorseLastAnm;
}

void putUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// HoZelda.arc indices ride +1 so that 0 means none.
int32_t zeldaAnm(uint16_t id) {
    return id == 0xFFFF ? 0 : id + 1;
}

uint16_t zeldaAnmFrom(int32_t v) {
    return v <= 0 || v > 0x100 ? 0xFFFF : static_cast<uint16_t>(v - 1);
}

void captureZelda(daHorse_c* h, RemoteHorsePose& out) {
    daHoZelda_c* z = h->getZeldaActor();
    if (z == nullptr || z->mpZeldaModel == nullptr) {
        return;
    }
    out.flags |= kHorseZelda;
    RemoteHorseZelda& r = out.zelda;
    const uint16_t ids[3] = {z->field_0x6e4[0], z->field_0x6e4[1], z->mUpperAnmID};
    for (int i = 0; i < 3; i++) {
        r.anm[i] = z->mAnmRatioPack[i].getAnmTransform() != nullptr ? ids[i] : 0xFFFF;
        r.frame[i] = z->mFrameCtrl[i].getFrame();
    }
    r.ratio = z->mAnmRatioPack[1].getRatio();
    r.upper = z->mBowMode != 0 || z->field_0x6da != 0;
    r.bowAnm = z->mBowAnmID;
    r.bowFrame = z->mBowBck.getFrame();
}

// daHorse_c::create's refusal
bool horseAwayInStory() {
    return dComIfGs_isEventBit(dSv_event_flag_c::M_008) &&
           !dComIfGs_isEventBit(dSv_event_flag_c::M_023);
}

}  // namespace

void resetSender() {
    s_tx = Sender{};
}

bool captureLocal(RemoteHorsePose& out) {
    out = RemoteHorsePose{};
    daHorse_c* h = dComIfGp_getHorseActor();
    // daHorse_c::draw's own early return
    if (h == nullptr || h->m_model == nullptr || h->m_modelData == nullptr ||
        h->checkStateFlg0(daHorse_c::FLG0_NO_DRAW_WAIT) ||
        h->checkResetStateFlg0(daHorse_c::RFLG0_UNK_80))
    {
        s_tx.present = false;
        return false;
    }
    const fpc_ProcID pid = fopAcM_GetID(h);
    const float dx = h->current.pos.x - s_tx.lastPos[0];
    const float dy = h->current.pos.y - s_tx.lastPos[1];
    const float dz = h->current.pos.z - s_tx.lastPos[2];
    if (!s_tx.present || pid != s_tx.pid ||
        dx * dx + dy * dy + dz * dz > kTeleportDistance * kTeleportDistance)
    {
        s_tx.epoch++;
    }
    s_tx.pid = pid;
    s_tx.present = true;
    s_tx.lastPos[0] = h->current.pos.x;
    s_tx.lastPos[1] = h->current.pos.y;
    s_tx.lastPos[2] = h->current.pos.z;

    out.flags = kHorsePresent;
    out.epoch = s_tx.epoch;
    if (h->checkStateFlg0(daHorse_c::FLG0_UNK_1)) {
        out.flags |= kHorseRidden;
    }
    // execute hides the bags on the model data at its end (J3DShpFlag_Visible set = hidden).
    if (h->m_modelData->getMaterialNum() > 5 &&
        h->m_modelData->getMaterialNodePointer(5)->getShape()->checkFlag(J3DShpFlag_Visible))
    {
        out.flags |= kHorseBagHidden;
    }
    if (h->checkResetStateFlg0(daHorse_c::RFLG0_UNK_100)) {
        out.flags |= kHorseReinsHidden;
    }
    if (h->checkResetStateFlg0(daHorse_c::RFLG0_UNK_1)) {
        out.flags |= kHorseReinReset;
    }
    captureZelda(h, out);
    const uint16_t anm0 = h->m_anmIdx[0];
    if ((anm0 != 0xFFFF && (anm0 & 0x8000)) || h->m_procID == daHorse_c::PROC_TOOL_DEMO_e) {
        out.flags |= kHorseDemo;
    }
    out.pos[0] = h->current.pos.x;
    out.pos[1] = h->current.pos.y;
    out.pos[2] = h->current.pos.z;
    out.angle[0] = h->shape_angle.x;
    out.angle[1] = h->shape_angle.y;
    out.angle[2] = h->shape_angle.z;
    for (int i = 0; i < 3; i++) {
        J3DAnmTransform* bck = h->m_anmRatio[i].getAnmTransform();
        const uint16_t id = h->m_anmIdx[i];
        if (bck != nullptr && validAnm(id)) {
            out.anm[i] = id;
            // The frame the calc used
            out.frame[i] = bck->getFrame();
        }
    }
    out.ratio[0] = h->m_anmRatio[0].getRatio();
    out.ratio[1] = h->m_anmRatio[1].getRatio();
    out.neckYaw = h->field_0x16f0;
    out.lean = h->field_0x16fa;
    for (int i = 0; i < 3; i++) {
        out.tail[i] = h->field_0x16d4[i];
    }
    for (int f = 0; f < 4; f++) {
        for (int j = 0; j < 4; j++) {
            out.foot[f][j] = h->m_footData[f].field_0x4[j];
        }
    }

    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link != nullptr && link->checkHorseRide() && link->mRideAcKeep.getActor() == h) {
        RemoteHorseRider& r = out.rider;
        r.active = true;
        // Link's calc ran in this execute with these
        r.rootMode = link->field_0x2f99;
        r.stirrups = link->field_0x2fab & 3;
        r.reinHand = static_cast<int8_t>(std::clamp(link->getReinHandType(), -1, 3));
        r.pitchComp = link->checkReinRide() && !link->checkHorseLieAnime() &&
                      link->mProcID != daAlink_c::PROC_HORSE_RUN;
        r.bowTilt = link->checkReinRide() && link->checkBowAnime();
        r.base[0] = link->field_0x3588.x;
        r.base[1] = link->field_0x33b0;
        r.base[2] = link->field_0x3588.z;
        Mtx inv;
        cMtx_inverse(h->getRootMtx(), inv);
        cXyz off;
        mDoMtx_multVec(inv, &link->current.pos, &off);
        r.off[0] = off.x;
        r.off[1] = off.y;
        r.off[2] = off.z;
    }
    return true;
}

void encode(const RemoteHorsePose& h, WirePose& w) {
    if (!h.present()) {
        return;
    }
    w.hsx[0] = (h.flags & kHorseWireMask) | (h.epoch << 8);
    w.hsx[1] = h.neckYaw;
    w.hsx[2] = h.lean;
    for (int i = 0; i < 3; i++) {
        w.hsx[3 + i] = h.tail[i];
        w.hsp[i] = quantize(h.pos[i], kPosScale);
        w.hss[i] = h.angle[i];
        w.hsa[i] = h.anm[i];
        w.hsf[i] = quantize(h.frame[i], kFrameScale);
    }
    w.hsw[0] = quantize(h.ratio[0], kRatioScale);
    w.hsw[1] = quantize(h.ratio[1], kRatioScale);
    for (int f = 0; f < 4; f++) {
        for (int j = 0; j < 4; j++) {
            w.hsk[f * 4 + j] = h.foot[f][j];
        }
    }
    const RemoteHorseRider& r = h.rider;
    if (r.active) {
        w.hsr[0] = 1 | (r.rootMode << 8) | ((r.stirrups & 3) << 16) |
                   ((r.reinHand + 1) << 20) | ((r.pitchComp ? 1 : 0) << 24) |
                   ((r.bowTilt ? 1 : 0) << 25);
        for (int i = 0; i < 3; i++) {
            w.hsr[1 + i] = quantize(r.base[i], kRiderBaseScale);
            w.hsr[4 + i] = quantize(r.off[i], kRiderOffsetScale);
        }
    }
    if (h.flags & kHorseZelda) {
        const RemoteHorseZelda& z = h.zelda;
        w.hsz[0] = zeldaAnm(z.anm[0]) | (zeldaAnm(z.anm[1]) << 16);
        w.hsz[1] = zeldaAnm(z.anm[2]) | (zeldaAnm(z.bowAnm) << 16);
        for (int i = 0; i < 3; i++) {
            w.hsz[2 + i] = quantize(z.frame[i], kFrameScale);
        }
        w.hsz[5] = quantize(z.ratio, kRatioScale);
        w.hsz[6] = quantize(z.bowFrame, kFrameScale);
        w.hsz[7] = z.upper ? 1 : 0;
    }
}

RemoteHorsePose decode(const WirePose& w) {
    RemoteHorsePose h;
    const uint32_t head = static_cast<uint32_t>(w.hsx[0]);
    const uint8_t flags = static_cast<uint8_t>(head & kHorseWireMask);
    if (!(flags & kHorsePresent)) {
        return h;  // nothing stale survives an absent horse
    }
    h.flags = flags;
    h.epoch = static_cast<uint8_t>((head >> 8) & 0xFF);
    h.neckYaw = static_cast<int16_t>(w.hsx[1]);
    h.lean = static_cast<int16_t>(w.hsx[2]);
    for (int i = 0; i < 3; i++) {
        h.tail[i] = static_cast<int16_t>(w.hsx[3 + i]);
        h.pos[i] = w.hsp[i] / kPosScale;
        h.angle[i] = static_cast<int16_t>(w.hss[i]);
        h.anm[i] = validAnm(w.hsa[i]) ? static_cast<uint16_t>(w.hsa[i]) : 0;
        h.frame[i] = h.anm[i] != 0 ? w.hsf[i] / kFrameScale : 0.0f;
        h.frameNext[i] = h.frame[i];
    }
    h.ratio[0] = std::clamp(w.hsw[0] / kRatioScale, 0.0f, 1.0f);
    h.ratio[1] = std::clamp(w.hsw[1] / kRatioScale, 0.0f, 1.0f);
    for (int f = 0; f < 4; f++) {
        for (int j = 0; j < 4; j++) {
            h.foot[f][j] = static_cast<int16_t>(w.hsk[f * 4 + j]);
        }
    }
    const uint32_t r0 = static_cast<uint32_t>(w.hsr[0]);
    if (r0 & 1) {
        RemoteHorseRider& r = h.rider;
        r.rootMode = static_cast<uint8_t>((r0 >> 8) & 0xFF);
        r.stirrups = static_cast<uint8_t>((r0 >> 16) & 3);
        r.reinHand = static_cast<int8_t>(std::clamp(static_cast<int>((r0 >> 20) & 7) - 1, -1, 3));
        r.pitchComp = ((r0 >> 24) & 1) != 0;
        r.bowTilt = ((r0 >> 25) & 1) != 0;
        bool sane = true;
        for (int i = 0; i < 3; i++) {
            r.base[i] = w.hsr[1 + i] / kRiderBaseScale;
            r.off[i] = w.hsr[4 + i] / kRiderOffsetScale;
            sane &= std::fabs(r.base[i]) <= kMaxRiderOffset && std::fabs(r.off[i]) <= kMaxRiderOffset;
        }
        r.active = sane;
        if (!sane) {
            r = RemoteHorseRider{};
        }
    }
    if (h.flags & kHorseZelda) {
        RemoteHorseZelda& z = h.zelda;
        z.anm[0] = zeldaAnmFrom(w.hsz[0] & 0xFFFF);
        z.anm[1] = zeldaAnmFrom((w.hsz[0] >> 16) & 0xFFFF);
        z.anm[2] = zeldaAnmFrom(w.hsz[1] & 0xFFFF);
        z.bowAnm = zeldaAnmFrom((w.hsz[1] >> 16) & 0xFFFF);
        for (int i = 0; i < 3; i++) {
            z.frame[i] = w.hsz[2 + i] / kFrameScale;
        }
        z.ratio = std::clamp(w.hsz[5] / kRatioScale, 0.0f, 1.0f);
        z.bowFrame = w.hsz[6] / kFrameScale;
        z.upper = (w.hsz[7] & 1) != 0;
        if (z.anm[0] == 0xFFFF) {
            h.flags &= ~kHorseZelda;  // her pack 0 always holds a clip
            z = RemoteHorseZelda{};
        }
    }
    return h;
}

std::string localNameUtf8() {
    const char* raw = dComIfGs_getHorseName();
    if (raw == nullptr) {
        return {};
    }
    // mHorseName is 17 bytes with the terminator.
    const size_t len = strnlen(raw, 16);
    const DVDDiskID* disk = DVDGetCurrentDiskID();
    const bool jpn = disk != nullptr && disk->gameName[3] == 'J';
    std::string out;
    for (size_t i = 0; i < len; i++) {
        const auto b = static_cast<uint8_t>(raw[i]);
        if (b >= 0x20 && b <= 0x7E) {
            out += static_cast<char>(b);
        } else if (jpn && i + 1 < len && ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC))) {
            const uint16_t sj = static_cast<uint16_t>(b << 8 | static_cast<uint8_t>(raw[i + 1]));
            i++;
            if (sj >= 0x829F && sj <= 0x82F1) {
                putUtf8(out, 0x3041 + (sj - 0x829F));
            } else if (sj >= 0x8340 && sj <= 0x8396 && sj != 0x837F) {
                putUtf8(out, 0x30A1 + (sj - 0x8340) - (sj > 0x837F ? 1 : 0));
            } else if (sj == 0x815B) {
                putUtf8(out, 0x30FC);  // the long vowel mark
            } else if (sj == 0x8140) {
                out += ' ';
            } else {
                out += '?';
            }
        } else if (!jpn && b >= 0xA0) {
            putUtf8(out, b);  // CP1252 and Latin-1 agree from 0xA0
        } else if (!jpn && b == 0x8C) {
            putUtf8(out, 0x0152);  // OE
        } else if (!jpn && b == 0x9C) {
            putUtf8(out, 0x0153);  // oe
        } else {
            out += '?';
        }
    }
    const size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = out.find_last_not_of(' ');
    return out.substr(first, last - first + 1);
}

bool localPlace(HorsePlace& out) {
    out = HorsePlace{};
    const char* stage = dComIfGs_getHorseRestartStageName();
    if (stage == nullptr || stage[0] == '\0' || horseAwayInStory()) {
        return false;
    }
    const cXyz pos = dComIfGs_getHorseRestartPos();
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
        return false;
    }
    out.valid = true;
    std::strncpy(out.stage, stage, sizeof(out.stage) - 1);
    out.room = dComIfGs_getHorseRestartRoomNo();
    out.pos[0] = pos.x;
    out.pos[1] = pos.y;
    out.pos[2] = pos.z;
    out.angleY = dComIfGs_getHorseRestartAngleY();
    return true;
}

const std::string& remoteName(const Client& c) {
    static const std::string kDefault = "Epona";  // message 0x383, the save's default
    return c.horseName.empty() ? kDefault : c.horseName;
}

}  // namespace twili::horse

