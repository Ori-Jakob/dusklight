// Steps for Epona sync; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyHorse.hpp"
#include "actors/DummyPlayer.hpp"
#include "core/GameAccess.hpp"
#include "core/Host.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "fx/PlayerRecolor.hpp"
#include "ui/NameTags.hpp"

#include "SSystem/SComponent/c_lib.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_horse.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"

#include <mods/svc/camera.h>

#include <fmt/format.h>

#include <algorithm>
#include <cfloat>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace twili::autotest {
namespace {

using nlohmann::json;
using Probe = ui::name_tags::Probe;

bool horseShown(daHorse_c* h) {
    return h != nullptr && h->m_model != nullptr &&
           !h->checkStateFlg0(daHorse_c::FLG0_NO_DRAW_WAIT) &&
           !h->checkResetStateFlg0(daHorse_c::RFLG0_UNK_80);
}

std::string horseText(daHorse_c* h) {
    if (h == nullptr) {
        return "none";
    }
    return fmt::format("pos ({:.0f}, {:.0f}, {:.0f}) yaw 0x{:04X} proc {} anm {}/{}/{} ride {} "
                       "callWait {}",
        h->current.pos.x, h->current.pos.y, h->current.pos.z, static_cast<u16>(h->shape_angle.y),
        h->m_procID, h->m_anmIdx[0], h->m_anmIdx[1], h->m_anmIdx[2],
        h->checkStateFlg0(daHorse_c::FLG0_UNK_1) != 0, h->checkHorseCallWait() != 0);
}

cXyz jointPos(J3DModel* model, int joint) {
    cXyz out;
    mDoMtx_multVecZero(model->getAnmMtx(joint), &out);
    return out;
}

// A step runs in whatever layer the main loop left; an actor in another is drawn twice.
class ScopedPlayerLayer {
public:
    explicit ScopedPlayerLayer(daAlink_c* link) : mSaved(fpcLy_CurrentLayer()) {
        fpcLy_SetCurrentLayer(link->layer_tag.layer);
    }
    ~ScopedPlayerLayer() { fpcLy_SetCurrentLayer(mSaved); }

private:
    layer_class* mSaved;
};

struct SpawnState {
    fpc_ProcID created = fpcM_ERROR_PROCESS_ID_e;
    bool placed = false;
};
SpawnState sSpawn;

bool spawnHorse(StepContext& ctx) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail("spawnHorse: no player");
        }
        return false;
    }
    const float dist = ctx.step.value("dist", 250.0f);
    const float side = ctx.step.value("side", 0.0f);
    const s16 yaw = link->shape_angle.y;
    cXyz pos(link->current.pos.x + dist * cM_ssin(yaw) - side * cM_scos(yaw), link->current.pos.y,
        link->current.pos.z + dist * cM_scos(yaw) + side * cM_ssin(yaw));
    daHorse_c* h = dComIfGp_getHorseActor();
    if (!ctx.begun) {
        sSpawn = SpawnState{};
        if (h == nullptr) {
            ScopedPlayerLayer layer(link);
            csXyz angle(0, yaw, 0);
            sSpawn.created = fopAcM_create(
                fpcNm_HORSE_e, 0x00FF, &pos, dComIfGp_roomControl_getStayNo(), &angle, nullptr, -1);
            TwiliLog.info(
                "[autotest] spawnHorse: no horse in the stage, created pid {}", sSpawn.created);
        } else {
            TwiliLog.info("[autotest] spawnHorse: stage horse {}", horseText(h));
        }
    }
    if (h != nullptr && !sSpawn.placed) {
        h->offNoDrawWait();
        h->setHorsePosAndAngle(&pos, yaw);
        h->initHorseMtx();
        sSpawn.placed = true;
        return false;
    }
    if (h != nullptr && fopAcM_IsExecuting(fopAcM_GetID(h)) && horseShown(h)) {
        TwiliLog.info("[autotest] horse ready: {}", horseText(h));
        return true;
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail(fmt::format("spawnHorse: no horse after {:.0f}s (created pid {}, {})", ctx.seconds,
            sSpawn.created, horseText(h)));
    }
    return false;
}

// Our Link `dist` from `horse`'s left flank, facing her (acceptPlayerRide wants him off her front).
void standBeside(daAlink_c* link, const fopAc_ac_c* horse, float dist) {
    const s16 yaw = horse->shape_angle.y;
    const cXyz pos(horse->current.pos.x + dist * cM_scos(yaw), horse->current.pos.y,
        horse->current.pos.z - dist * cM_ssin(yaw));
    link->current.pos = pos;
    link->old.pos = pos;
    link->shape_angle.y = static_cast<s16>(yaw - 0x4000);
    link->current.angle.y = link->shape_angle.y;
    interp::requestPresentationSync();
}

bool rideHorse(StepContext& ctx) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    daHorse_c* h = dComIfGp_getHorseActor();
    const bool ride = ctx.step.value("ride", true);
    const std::string mode = ctx.step.value("mode", std::string("force"));
    if (link == nullptr || h == nullptr) {
        ctx.fail("rideHorse: no player or no horse");
        return false;
    }
    if (!ctx.begun) {
        if (ride && mode == "mount") {
            standBeside(link, h, 110.0f);
            link->field_0x27f4 = h;
            link->procHorseRideInit();
        } else if (ride) {
            h->setHorsePosAndAngle(&link->current.pos, link->shape_angle.y);
            h->initHorseMtx();
            link->initForceRideHorse();
            link->procHorseWaitInit();
        } else {
            link->procHorseGetOffInit(0);
        }
        TwiliLog.info("[autotest] rideHorse {} {}: proc 0x{:X}", ride ? "on" : "off", mode,
            static_cast<int>(link->mProcID));
    }
    const bool riding = link->checkHorseRide() != 0;
    const bool moving = link->mProcID == daAlink_c::PROC_HORSE_RIDE ||
                        link->mProcID == daAlink_c::PROC_HORSE_GETOFF;
    if (riding == ride && !moving) {
        TwiliLog.info("[autotest] rideHorse done after {} ticks: riding {} proc 0x{:X} root "
                      "0x{:X} horse {}",
            ctx.ticks, riding, static_cast<int>(link->mProcID), link->field_0x2f99, horseText(h));
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("rideHorse: riding {} proc 0x{:X} after {:.0f}s", riding,
            static_cast<int>(link->mProcID), ctx.seconds));
    }
    return false;
}

bool expectLocalHorse(StepContext& ctx) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    daHorse_c* h = dComIfGp_getHorseActor();
    const bool present = horseShown(h);
    const bool ridden = present && link != nullptr && link->checkHorseRide() &&
                        h->checkStateFlg0(daHorse_c::FLG0_UNK_1);
    std::string why;
    if (present != ctx.step.value("present", true)) {
        why = fmt::format("present {}", present);
    } else if (ctx.step.contains("ridden") && ridden != ctx.step.value("ridden", false)) {
        why = fmt::format("ridden {}", ridden);
    }
    if (why.empty()) {
        std::string seat;
        if (ridden) {
            seat = fmt::format(
                " seat {:.1f}", jointPos(link->mpLinkModel, 0).abs(jointPos(h->m_model, 21)));
        }
        TwiliLog.info("[autotest] local horse ok: {}{}", horseText(h), seat);
        return true;
    }
    if (ctx.seconds > ctx.timeout(5.0)) {
        ctx.fail(fmt::format("expectLocalHorse: {} ({})", why, horseText(h)));
    }
    return false;
}

bool setHorseHidden(StepContext& ctx) {
    daHorse_c* h = dComIfGp_getHorseActor();
    if (h == nullptr) {
        ctx.fail("setHorseHidden: no horse");
        return false;
    }
    if (ctx.step.value("hidden", true)) {
        h->onStateFlg0(daHorse_c::FLG0_NO_DRAW_WAIT);
        // Her execute returns early from now on and would keep offering the ride it offered last.
        h->attention_info.flags &= ~(fopAc_AttnFlag_ETC_e | fopAc_AttnFlag_SPEAK_e);
    } else {
        h->offNoDrawWait();
    }
    TwiliLog.info("[autotest] horse hidden {}", h->checkHorseCallWait() != 0);
    return true;
}

// The key updateHorseRecolor gives the mane for a player colour: none for a grey.
uint32_t maneKey(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t key = recolorKey(r, g, b);
    if (key != RecolorSet::kPristine && toHsv({r / 255.0f, g / 255.0f, b / 255.0f}).s < 0.10f) {
        key = RecolorSet::kPristine;
    }
    return key;
}

std::string drawnText() {
    std::string out;
    for (const auto& t : ui::name_tags::drawn()) {
        out += fmt::format(" [{} {} \"{}\"/\"{}\"]", t.kind == Probe::Kind::Player ? "tag" : "card",
            t.clientId, t.line1, t.line2);
    }
    return out.empty() ? std::string(" nothing") : out;
}

const Probe* drawnTag(uint32_t id, Probe::Kind kind) {
    for (const auto& t : ui::name_tags::drawn()) {
        if (t.clientId == id && t.kind == kind) {
            return &t;
        }
    }
    return nullptr;
}

// A puppet plays out a few ticks behind the newest update: compared with the recent positions.
std::map<uint32_t, std::deque<cXyz>> sHorseTrail;
constexpr size_t kHorseTrailLength = 120;

void noteHorseTrail(uint32_t id, const Client& c) {
    std::deque<cXyz>& trail = sHorseTrail[id];
    if (!c.horse.present()) {
        trail.clear();
        return;
    }
    trail.emplace_back(c.horse.pos[0], c.horse.pos[1], c.horse.pos[2]);
    if (trail.size() > kHorseTrailLength) {
        trail.pop_front();
    }
}

float trailDist(uint32_t id, const cXyz& pos) {
    float best = FLT_MAX;
    for (const cXyz& p : sHorseTrail[id]) {
        best = std::min(best, pos.abs(p));
    }
    return best;
}

// What of the step's expectations `id`'s puppet misses, empty when none.
std::string horseMismatch(const json& step, uint32_t id, const Client& c, fopAc_ac_c* actor,
    const DummyHorseDebugInfo& d, float& seat) {
    if (actor == dComIfGp_getHorseActor()) {
        return "the puppet is our own horse";
    }
    if (!d.shown) {
        return fmt::format("not shown (posed {} parked {} demo {})", d.posed, d.parked, d.demoHeld);
    }
    if (d.attentionFlags != 0) {
        return fmt::format("attention flags 0x{:X}", d.attentionFlags);
    }
    if (d.refused != 0) {
        return fmt::format("{} clips refused", d.refused);
    }
    const cXyz pos(d.pos[0], d.pos[1], d.pos[2]);
    if (step.contains("parked") && d.parked != step.value("parked", false)) {
        return fmt::format("parked {}", d.parked);
    }
    if (d.parked) {
        const cXyz place(c.horsePlace.pos[0], c.horsePlace.pos[1], c.horsePlace.pos[2]);
        if (pos.abs(place) > 50.0f) {
            return fmt::format("{:.0f} from the horse place", pos.abs(place));
        }
    } else {
        const float dist = trailDist(id, pos);
        if (!c.horse.present() || dist > step.value("maxDist", 400.0f)) {
            return fmt::format(
                "{:.0f} from the latest horse positions (present {})", dist, c.horse.present());
        }
    }
    if (step.contains("ridden") && d.ridden != step.value("ridden", false)) {
        const RemoteHorseRider& r = c.horse.rider;
        return fmt::format("ridden {} (latest flags 0x{:X} rider {} mode 0x{:X})", d.ridden,
            c.horse.flags, r.active, r.rootMode);
    }
    if (d.ridden) {
        fopAc_ac_c* dummy = Session::instance().dummyActorForClient(id);
        if (dummy == nullptr) {
            return "ridden without a dummy";
        }
        seat = jointPos(static_cast<daAlink_c*>(dummy)->mpLinkModel, 0)
                   .abs(cXyz(d.saddlePos[0], d.saddlePos[1], d.saddlePos[2]));
        if (seat > step.value("maxSeatDist", 60.0f)) {
            return fmt::format("the rider's root is {:.0f} from the saddle", seat);
        }
    }
    const bool onScreen = step.value("onScreen", false);
    if (step.contains("card")) {
        const json& want = step["card"];
        const std::string card = ui::name_tags::horseCardText(id);
        const std::string wantText = want.is_string() ? want.get<std::string>() : std::string{};
        if (card != wantText) {
            return fmt::format("card \"{}\", want \"{}\"", card, wantText);
        }
        const Probe* drawn = drawnTag(id, Probe::Kind::HorseCard);
        if (onScreen && !wantText.empty() && (drawn == nullptr || drawn->line1 != wantText)) {
            return fmt::format("card not on screen (drawn:{})", drawnText());
        }
    }
    if (step.contains("tagLine2")) {
        const std::string want = step.value("tagLine2", std::string{});
        const std::string line2 = ui::name_tags::riderTagLine2(id);
        if (line2 != want) {
            return fmt::format("tag line 2 \"{}\", want \"{}\"", line2, want);
        }
        const Probe* drawn = drawnTag(id, Probe::Kind::Player);
        if (onScreen && (drawn == nullptr || drawn->line2 != want)) {
            return fmt::format("tag not on screen (drawn:{})", drawnText());
        }
    }
    if (step.value("vanilla", false) && d.recolorKey != RecolorSet::kPristine) {
        return fmt::format("mane holds #{:06X}", d.recolorKey);
    }
    if (step.value("tint", false)) {
        const uint32_t want = maneKey(c.colorR, c.colorG, c.colorB);
        if (!d.recolorBound) {
            return "mane recolour not bound";
        }
        if (d.recolorKey != want) {
            return fmt::format("mane holds key 0x{:08X}, want 0x{:08X}", d.recolorKey, want);
        }
        const Hsv owner = toHsv({c.colorR / 255.0f, c.colorG / 255.0f, c.colorB / 255.0f});
        if (want != RecolorSet::kPristine && owner.s >= 0.2f) {
            const float gain = hueDist(d.manePristine.h, owner.h) - hueDist(d.mane.h, owner.h);
            if (gain < step.value("hueGain", 1.0f)) {
                return fmt::format("mane hue {:.1f} (on disc {:.1f}) did not move toward {:.1f}",
                    d.mane.h, d.manePristine.h, owner.h);
            }
        }
    }
    return {};
}

int sMatchedAt = -1;

bool expectRemoteHorse(StepContext& ctx) {
    if (!ctx.begun) {
        sMatchedAt = -1;
        sHorseTrail.clear();
    }
    const json& step = ctx.step;
    const std::string name = step.value("name", std::string{});
    const int count = step.value("count", 1);
    int ok = 0;
    std::string why = "no puppet";
    std::vector<std::string> lines;
    for (const auto& [id, c] : Session::instance().clients()) {
        if (c.self || (!name.empty() && c.name != name)) {
            continue;
        }
        noteHorseTrail(id, c);
        fopAc_ac_c* actor = Session::instance().horseActorForClient(id);
        DummyHorseDebugInfo d;
        if (!GetDummyHorseDebugInfo(actor, d)) {
            why = fmt::format("{}: no puppet", c.name);
            continue;
        }
        float seat = -1.0f;
        const std::string miss = horseMismatch(step, id, c, actor, d, seat);
        if (!miss.empty()) {
            why = fmt::format("{}: {}", c.name, miss);
            continue;
        }
        ok++;
        lines.push_back(
            fmt::format("{} {} anm {}/{}/{} ridden {} seat {:.1f} reins {} mane h{:.0f} "
                        "s{:.2f} (disc h{:.0f}) key 0x{:08X} sounds {} heap 0x{:X} "
                        "card \"{}\" tag2 \"{}\"",
                c.name, d.parked ? "parked" : "live", d.anm[0], d.anm[1], d.anm[2], d.ridden, seat,
                d.reinPoints, d.mane.h, d.mane.s, d.manePristine.h, d.recolorKey, d.soundAnims,
                d.heapUsed, ui::name_tags::horseCardText(id), ui::name_tags::riderTagLine2(id)));
    }
    if (ok >= count) {
        if (sMatchedAt < 0) {
            sMatchedAt = ctx.ticks;
        }
        if (ctx.ticks - sMatchedAt >= step.value("frames", 0)) {
            for (const std::string& l : lines) {
                TwiliLog.info("[autotest] remote horse {}", l);
            }
            return true;
        }
        return false;
    }
    if (sMatchedAt >= 0) {
        ctx.fail(fmt::format(
            "expectRemoteHorse: stopped holding after {} ticks: {}", ctx.ticks - sMatchedAt, why));
        return false;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("expectRemoteHorse: {} of {} ({})", ok, count, why));
    }
    return false;
}

bool expectNoRemoteHorse(StepContext& ctx) {
    std::string why;
    for (const auto& [id, c] : Session::instance().clients()) {
        DummyHorseDebugInfo d;
        if (!c.self && GetDummyHorseDebugInfo(Session::instance().horseActorForClient(id), d) &&
            d.shown)
        {
            why = fmt::format("{}'s horse is shown", c.name);
        }
        if (drawnTag(id, Probe::Kind::HorseCard) != nullptr) {
            why = fmt::format("{}'s horse card is drawn", c.name);
        }
    }
    if (why.empty()) {
        TwiliLog.info("[autotest] no remote horse shown");
        return true;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail("expectNoRemoteHorse: " + why);
    }
    return false;
}

daDummyHorse_c* firstRemoteHorse() {
    for (const auto& [id, c] : Session::instance().clients()) {
        daDummyHorse_c* horse = c.self ? nullptr : FindDummyHorse(id);
        if (horse != nullptr && horse->isShown()) {
            return horse;
        }
    }
    return nullptr;
}

bool isPuppet(const fopAc_ac_c* actor) {
    return actor != nullptr && g_procDummyHorse >= 0 &&
           fopAcM_GetName(const_cast<fopAc_ac_c*>(actor)) == g_procDummyHorse;
}

bool expectRidePrompt(StepContext& ctx) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    const bool own = ctx.step.value("target", std::string("remote")) == "own";
    daHorse_c* ours = dComIfGp_getHorseActor();
    fopAc_ac_c* target = own ? static_cast<fopAc_ac_c*>(ours) : firstRemoteHorse();
    if (link == nullptr || target == nullptr) {
        ctx.fail(fmt::format("expectRidePrompt: no player or no {} horse", own ? "own" : "remote"));
        return false;
    }
    if (!ctx.begun) {
        standBeside(link, target, ctx.step.value("dist", 110.0f));
    }
    const bool getOn = dComIfGp_getDoStatus() == BUTTON_STATUS_GET_ON;
    const int frames = ctx.step.value("frames", 60);
    if (own) {
        if (getOn && link->field_0x27f4 == target) {
            TwiliLog.info("[autotest] ride prompt on our horse after {} ticks", ctx.ticks);
            return true;
        }
        if (ctx.ticks > frames) {
            ctx.fail(fmt::format("expectRidePrompt: no GET_ON on our horse (do status {}, "
                                 "offered {})",
                dComIfGp_getDoStatus(), static_cast<void*>(link->field_0x27f4)));
        }
        return false;
    }
    // The prompt can only offer the actor in field_0x27f4 (a GET_ON may be our own horse's).
    if (link->field_0x27f4 == target || link->mRideAcKeep.getActor() == target) {
        ctx.fail(fmt::format("expectRidePrompt: the remote horse is offered (do status {}, "
                             "offered {} ride {})",
            dComIfGp_getDoStatus(), link->field_0x27f4 == target,
            link->mRideAcKeep.getActor() == target));
        return false;
    }
    if (ctx.ticks >= frames) {
        TwiliLog.info("[autotest] no ride prompt on the remote horse for {} ticks", ctx.ticks);
        return true;
    }
    return false;
}

bool expectNotRiding(StepContext& ctx) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        ctx.fail("expectNotRiding: no player");
        return false;
    }
    if (link->checkHorseRide() || isPuppet(link->mRideAcKeep.getActor())) {
        ctx.fail(fmt::format("expectNotRiding: riding (horse {}, proc 0x{:X})",
            link->checkHorseRide() != 0, static_cast<int>(link->mProcID)));
        return false;
    }
    return ctx.ticks >= ctx.step.value("frames", 30);
}

// The held view: our operator answers the main camera with it until a release.
struct CameraHold {
    CameraOperatorHandle handle = 0;
    cXyz center = cXyz::Zero;
    cXyz eye = cXyz::Zero;
};
CameraHold sHold;

bool operateHeldCamera(ModContext*, CameraOperatorState* state, void*) {
    state->center[0] = sHold.center.x;
    state->center[1] = sHold.center.y;
    state->center[2] = sHold.center.z;
    state->eye[0] = sHold.eye.x;
    state->eye[1] = sHold.eye.y;
    state->eye[2] = sHold.eye.z;
    return true;
}

void releaseCamera() {
    if (sHold.handle != 0 && svc_camera != nullptr) {
        svc_camera->unregister_camera_operator(mod_ctx, sHold.handle);
    }
    sHold.handle = 0;
}

// Turns our Link toward the target where he stands and computes the view behind him.
bool aimCamera(const json& cam, cXyz& center, cXyz& eye, std::string& why) {
    const std::string target = cam.value("target", std::string("remote"));
    daAlink_c* link = daAlink_getAlinkActorClass();
    daHorse_c* ours = dComIfGp_getHorseActor();
    daDummyHorse_c* remote = firstRemoteHorse();
    const fopAc_ac_c* anchor = target == "own" ? static_cast<fopAc_ac_c*>(ours) : remote;
    if (link == nullptr || anchor == nullptr ||
        (target == "both" && (ours == nullptr || remote == nullptr)))
    {
        why = "no player or horse to look at";
        return false;
    }
    center = anchor->current.pos;
    if (target == "both") {
        center = (ours->current.pos + remote->current.pos) * 0.5f;
    }
    const cXyz stand = link->current.pos;
    const s16 yaw = cLib_targetAngleY(&stand, &center);
    link->shape_angle.y = yaw;
    link->current.angle.y = yaw;
    const cXyz front(cM_ssin(yaw), 0.0f, cM_scos(yaw));
    const cXyz right(front.z, 0.0f, -front.x);
    center.y += cam.value("height", 130.0f);
    eye = stand - front * cam.value("back", 250.0f) + right * cam.value("side", 0.0f) +
          cXyz(0.0f, cam.value("up", 200.0f), 0.0f);
    return true;
}

bool horseCamera(StepContext& ctx) {
    if (ctx.step.value("release", false)) {
        releaseCamera();
        return true;
    }
    cXyz center;
    cXyz eye;
    std::string why;
    if (!aimCamera(ctx.step, center, eye, why)) {
        ctx.fail("horseCamera: " + why);
        return false;
    }
    if (!ctx.step.value("hold", false)) {
        camera_process_class* camera = dComIfGp_getCamera(dComIfGp_getPlayerCameraID(0));
        if (camera == nullptr) {
            ctx.fail("horseCamera: no camera");
            return false;
        }
        camera->mCamera.Reset(center, eye);
        // A cut like the game's own: no presented frame lerps across it.
        interp::requestPresentationSync();
        return true;
    }
    if (!SERVICE_HAS(svc_camera, CameraService, register_camera_operator)) {
        ctx.fail("horseCamera: hold needs CameraService 1.1");
        return false;
    }
    sHold.center = center;
    sHold.eye = eye;
    if (sHold.handle == 0) {
        CameraOperatorDesc desc = CAMERA_OPERATOR_DESC_INIT;
        desc.debug_name = "twili autotest horseCamera";
        desc.operate = operateHeldCamera;
        if (svc_camera->register_camera_operator(mod_ctx, &desc, &sHold.handle) != MOD_OK) {
            sHold.handle = 0;
            ctx.fail("horseCamera: could not register the camera operator");
            return false;
        }
    }
    interp::requestPresentationSync();
    return true;
}

bool clearEnemies(StepContext& ctx) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        ctx.fail("clearEnemies: no player");
        return false;
    }
    struct Search {
        cXyz center;
        float radiusSq;
        std::vector<fpc_ProcID> found;
    } search{link->current.pos, 0.0f, {}};
    const float radius = ctx.step.value("radius", 8000.0f);
    search.radiusSq = radius * radius;
    fopAcIt_Executor(
        [](void* p, void* data) -> int {
            auto* ac = static_cast<fopAc_ac_c*>(p);
            auto* s = static_cast<Search*>(data);
            if (fopAcM_GetGroup(ac) == fopAc_ENEMY_e &&
                ac->current.pos.abs2(s->center) <= s->radiusSq) {
                s->found.push_back(fopAcM_GetID(ac));
            }
            return 1;
        },
        &search);
    for (fpc_ProcID id : search.found) {
        fopAcM_delete(id);
    }
    TwiliLog.info("[autotest] cleared {} enemies within {:.0f}", search.found.size(), radius);
    return true;
}

std::optional<bool> horseSteps(const std::string& op, StepContext& ctx) {
    if (op == "spawnHorse") {
        return spawnHorse(ctx);
    }
    if (op == "rideHorse") {
        return rideHorse(ctx);
    }
    if (op == "expectLocalHorse") {
        return expectLocalHorse(ctx);
    }
    if (op == "setHorseHidden") {
        return setHorseHidden(ctx);
    }
    if (op == "expectRemoteHorse") {
        return expectRemoteHorse(ctx);
    }
    if (op == "expectNoRemoteHorse") {
        return expectNoRemoteHorse(ctx);
    }
    if (op == "expectRidePrompt") {
        return expectRidePrompt(ctx);
    }
    if (op == "expectNotRiding") {
        return expectNotRiding(ctx);
    }
    if (op == "horseCamera") {
        return horseCamera(ctx);
    }
    if (op == "clearEnemies") {
        return clearEnemies(ctx);
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&horseSteps);

}  // namespace
}  // namespace twili::autotest
