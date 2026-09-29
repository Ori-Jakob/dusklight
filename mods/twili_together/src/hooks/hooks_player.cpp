#include "hooks/Hooks.hpp"
#include "hooks/Perf.hpp"

#include "core/GameAccess.hpp"
#include "core/Session.hpp"
#include "fx/ItemFx.hpp"
#include "presence/Presence.hpp"

#include "Z2AudioLib/Z2Creature.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_midna.h"
#include "f_op/f_op_actor_mng.h"

namespace twili::hooks {

DEFINE_HOOK(&daAlink_c::execute, LinkExecute);
DEFINE_HOOK(&daAlink_c::voiceStart, LinkVoiceStart);
DEFINE_HOOK(&daAlink_c::voiceStartLevel, LinkVoiceStartLevel);
DEFINE_HOOK(&daAlink_c::seStartSwordCut, LinkSeStartSwordCut);
DEFINE_HOOK(&daAlink_c::seStartOnlyReverb, LinkSeStartOnlyReverb);
DEFINE_HOOK(&daAlink_c::seStartOnlyReverbLevel, LinkSeStartOnlyReverbLevel);
DEFINE_HOOK(&daAlink_c::seStartMapInfo, LinkSeStartMapInfo);
DEFINE_HOOK(&daAlink_c::seStartMapInfoLevel, LinkSeStartMapInfoLevel);
DEFINE_HOOK(&Z2Creature::startCreatureVoice, CreatureVoice);
DEFINE_HOOK(&Z2Creature::startCreatureSound, CreatureSound);

namespace {

constexpr f32 kRemoteSfxMaxDistance = 3500.0f;

// Dummies run daAlink_c's sound code too; only the near ones are heard.
HookAction onLinkSfxPre(ModContext*, void* args, void*, void*) {
    perf::count(perf::Target::LinkSfx);
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (isDummyPlayer(link) &&
        fopAcM_searchPlayerDistanceXZ2(link) > kRemoteSfxMaxDistance * kRemoteSfxMaxDistance)
    {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

bool isLocalSender(daAlink_c* link) {
    return link != nullptr && link == localLink() && Session::active() &&
           Session::instance().isConnected();
}

template <PlayerSfxKind Kind, bool WithMapInfo>
void onLinkSfxPost(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (!isLocalSender(link)) {
        return;
    }
    Session::instance().sendPlayerSfx(
        mods::arg<u32>(args, 1), Kind, WithMapInfo ? link->mPolySound : 0);
}

// Looping sounds last only while started every tick: they ride in PLAYER_UPDATE "ls".
template <PlayerSfxKind Kind, bool WithMapInfo>
void onLinkLevelSfxPost(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (!isLocalSender(link)) {
        return;
    }
    itemfx::noteLevelSfx(
        mods::arg<u32>(args, 1), static_cast<uint8_t>(Kind), WithMapInfo ? link->mPolySound : 0);
}

// Midna's one-shots (her voices, jumps, warps); her clips' own sounds replay on the dummy.
template <PlayerSfxKind Kind>
void onCreatureSfxPost(ModContext*, void* args, void*, void*) {
    const auto* creature = mods::arg<Z2Creature*>(args, 0);
    const daMidna_c* midna = daPy_py_c::getMidnaActor();
    if (midna == nullptr || creature != &midna->mSound || !Session::active() ||
        !Session::instance().isConnected())
    {
        return;
    }
    const u32 id = mods::arg<JAISoundID>(args, 1);
    const u32 mapInfo = Kind == PlayerSfxKind::MidnaSound ? mods::arg<u32>(args, 2) : 0;
    Session::instance().sendPlayerSfx(id, Kind, mapInfo);
}

void onLinkExecutePost(ModContext*, void* args, void*, void*) {
    perf::count(perf::Target::LinkExecute);
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (isLocalSender(link)) {
        presence::captureAndSend(link);
    }
}

template <class Entry>
ModResult addSfx(HookPostFn post, const char* what, std::string& error) {
    ModResult result = addPre<Entry>(onLinkSfxPre, kDefault, what, error);
    if (result == MOD_OK) {
        result = addPost<Entry>(post, kDefault, what, error);
    }
    return result;
}

}  // namespace

ModResult installPlayer(std::string& error) {
    ModResult result = addPost<LinkExecute>(onLinkExecutePost, kDefault, "daAlink_c::execute", error);
    if (result != MOD_OK) {
        return result;
    }
    const ModResult results[] = {
        addSfx<LinkVoiceStart>(onLinkSfxPost<PlayerSfxKind::Voice, false>, "voiceStart", error),
        addSfx<LinkVoiceStartLevel>(onLinkLevelSfxPost<PlayerSfxKind::VoiceLevel, false>,
            "voiceStartLevel", error),
        addSfx<LinkSeStartSwordCut>(
            onLinkSfxPost<PlayerSfxKind::Sword, false>, "seStartSwordCut", error),
        addSfx<LinkSeStartOnlyReverb>(
            onLinkSfxPost<PlayerSfxKind::Sound, false>, "seStartOnlyReverb", error),
        addSfx<LinkSeStartOnlyReverbLevel>(onLinkLevelSfxPost<PlayerSfxKind::SoundLevel, false>,
            "seStartOnlyReverbLevel", error),
        addSfx<LinkSeStartMapInfo>(
            onLinkSfxPost<PlayerSfxKind::MapInfo, true>, "seStartMapInfo", error),
        addSfx<LinkSeStartMapInfoLevel>(onLinkLevelSfxPost<PlayerSfxKind::MapInfoLevel, true>,
            "seStartMapInfoLevel", error),
        addPost<CreatureVoice>(onCreatureSfxPost<PlayerSfxKind::MidnaVoice>, kDefault,
            "Z2Creature::startCreatureVoice", error),
        addPost<CreatureSound>(onCreatureSfxPost<PlayerSfxKind::MidnaSound>, kDefault,
            "Z2Creature::startCreatureSound", error),
    };
    for (const ModResult r : results) {
        if (r != MOD_OK) {
            return r;
        }
    }
    return MOD_OK;
}

}  // namespace twili::hooks
