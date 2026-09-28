// Steps for team games and the randomizer game identity; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"
#include "game/GameIdentity.hpp"
#include "game/TeamGame.hpp"
#include "teleport/Teleport.hpp"
#include "ui/ColorMath.hpp"

#include <mods/svc/item.h>

#include <fmt/format.h>

#include <map>

namespace twili::autotest {
namespace {

using nlohmann::json;

// Stands in for a loaded randomizer seed: answers the checks the fixture seed names.
std::map<std::string, uint8_t> sFakeItems;
ItemCheckHandle sFakeResolver = 0;

bool fakeResolve(ModContext*, const ItemCheckInfo* info, ItemCheckResolution* out, void*) {
    const auto it = sFakeItems.find(info->name);
    if (it == sFakeItems.end()) {
        return false;
    }
    out->item = it->second;
    out->display_item = 0xFF;
    out->was_resolved = true;
    return true;
}

const Client* findPeer(const std::string& name) {
    for (const auto& [id, c] : Session::instance().clients()) {
        if (!c.self && c.online && c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

bool waitFor(StepContext& ctx, bool done, double timeout, const std::string& what) {
    if (done) {
        return true;
    }
    if (ctx.seconds > ctx.timeout(timeout)) {
        ctx.fail(what);
    }
    return false;
}

std::string describeLocal() {
    const game_identity::Identity& id = game_identity::current();
    return fmt::format("kind={} key={} name=\"{}\" verified={} permalink={}", id.kind, id.key,
        id.name, id.verified, id.permalink.empty() ? "none" : "set");
}

std::string describeTeam(const team_game::Team* team) {
    if (team == nullptr) {
        return "no team state";
    }
    return fmt::format("owner={} game={} key={} name=\"{}\" permalink={}", team->ownerClientId,
        team_game::gameLabel(team->game), team->game.key, team->game.name,
        team->game.permalink.empty() ? "none" : "set");
}

std::optional<bool> teamGameSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    // Before connect: announce this identity instead of the detected one ({} restores detection).
    if (op == "setGameIdentity") {
        json identity = step.value("identity", json::object());
        game_identity::setOverrideForTest(identity);
        TwiliLog.info("[autotest] game identity forced: {}", identity.dump());
        return true;
    }

    // items: {checkName: itemNo}; clear: true removes it.
    if (op == "fakeRandoResolver") {
        if (svc_item == nullptr) {
            ctx.fail("ItemService is unavailable");
            return false;
        }
        if (sFakeResolver != 0) {
            svc_item->clear_check_resolver(mod_ctx, sFakeResolver);
            sFakeResolver = 0;
        }
        sFakeItems.clear();
        if (!step.value("clear", false)) {
            const json items = step.value("items", json::object());
            for (const auto& [name, item] : items.items()) {
                sFakeItems[name] = static_cast<uint8_t>(item.get<int>());
            }
            TwiliLog.info("[autotest] check resolver answers {} check(s)", sFakeItems.size());
            if (svc_item->set_check_resolver(
                    mod_ctx, nullptr, fakeResolve, nullptr, &sFakeResolver) != MOD_OK)
            {
                ctx.fail("could not register the check resolver");
                return false;
            }
        }
        game_identity::invalidate();
        return true;
    }

    if (op == "expectLocalGame") {
        const game_identity::Identity& id = game_identity::current();
        const std::string prefix = step.value("keyPrefix", std::string{});
        bool ok = id.key.rfind(prefix, 0) == 0;
        ok = ok && (!step.contains("kind") || id.kind == step.value("kind", std::string{}));
        ok = ok && (!step.contains("key") || id.key == step.value("key", std::string{}));
        ok = ok && (!step.contains("name") || id.name == step.value("name", std::string{}));
        ok = ok && (!step.contains("verified") || id.verified == step.value("verified", true));
        ok = ok && (!step.contains("permalink") ||
                       id.permalink == step.value("permalink", std::string{}));
        if (ok) {
            TwiliLog.info("[autotest] local game {}", describeLocal());
        }
        return waitFor(ctx, ok, 60.0, "local game is " + describeLocal());
    }

    if (op == "expectSyncState") {
        const std::string want = step.value("state", std::string("ok"));
        const char* got = team_game::syncName(team_game::localSync());
        if (want == got) {
            TwiliLog.info("[autotest] sync state {}", got);
        }
        return waitFor(ctx, want == got, 60.0, fmt::format("sync state is {}, want {}", got, want));
    }

    if (op == "expectMemberSync") {
        const std::string name = step.value("peer", std::string{});
        const std::string want = step.value("state", std::string("ok"));
        const Client* peer = findPeer(name);
        const team_game::Member* m = peer != nullptr ? team_game::member(peer->clientId) : nullptr;
        const std::string got = m != nullptr ? team_game::syncName(m->sync) : "unknown";
        return waitFor(
            ctx, got == want, 60.0, fmt::format("{} sync state is {}, want {}", name, got, want));
    }

    // Our team, or `team`; key, name, kind, owner (a peer name or "self"), permalink, noPermalink.
    if (op == "expectTeamGame") {
        const team_game::Team* team = team_game::ownTeam();
        if (step.contains("team")) {
            const auto it = team_game::teams().find(step.value("team", std::string{}));
            team = it == team_game::teams().end() ? nullptr : &it->second;
        }
        bool ok = team != nullptr;
        if (ok && step.contains("key")) {
            ok = team->game.key == step.value("key", std::string{});
        }
        if (ok && step.contains("keyPrefix")) {
            ok = team->game.key.rfind(step.value("keyPrefix", std::string{}), 0) == 0;
        }
        if (ok && step.contains("kind")) {
            ok = team->game.kind == step.value("kind", std::string{});
        }
        if (ok && step.contains("name")) {
            ok = team->game.name == step.value("name", std::string{});
        }
        if (ok && step.contains("permalink")) {
            ok = team->game.permalink == step.value("permalink", std::string{});
        }
        if (ok && step.value("noPermalink", false)) {
            ok = team->game.permalink.empty();
        }
        if (ok && step.value("noKey", false)) {
            ok = team->game.key.empty();
        }
        if (ok && step.contains("sameGame")) {
            ok = team->sameGameAsYours == step.value("sameGame", false);
        }
        if (ok && step.contains("owner")) {
            const std::string owner = step.value("owner", std::string{});
            const Client* peer = owner == "self" ? nullptr : findPeer(owner);
            const uint32_t want = owner == "self" ? Session::instance().selfClientId() :
                                                    (peer != nullptr ? peer->clientId : 0);
            ok = want != 0 && team->ownerClientId == want;
        }
        if (ok) {
            TwiliLog.info("[autotest] team game {}", describeTeam(team));
        }
        return waitFor(ctx, ok, 60.0, "team game is " + describeTeam(team));
    }

    // What Copy Permalink puts on the clipboard (the clipboard itself is left alone).
    if (op == "expectCopyText") {
        const std::string want = step.value("text", std::string{});
        const std::string& got = team_game::ownPermalink();
        return waitFor(ctx, got == want, 30.0, fmt::format("copy text is \"{}\"", got));
    }

    if (op == "claimTeamGame") {
        team_game::claimTeamGame();
        return true;
    }

    if (op == "confirmUnverified") {
        team_game::allowUnverified();
        return true;
    }

    // The Players tab's "Make ... Room Owner" / "Make ... Team Leader" after its confirm.
    if (op == "promote") {
        const std::string name = step.value("peer", std::string{});
        const Client* peer = findPeer(name);
        if (peer == nullptr) {
            ctx.fail("no peer " + name);
            return false;
        }
        if (step.value("role", std::string("room")) == "team") {
            team_game::promoteTeamLeader(peer->clientId);
        } else {
            team_game::promoteRoomOwner(peer->clientId);
        }
        return true;
    }

    // The Room tab's Team Colour picker (team leader only).
    if (op == "setTeamColor") {
        const auto rgb = ui::color::parseHex(step.value("rgb", std::string{}));
        if (!rgb || !team_game::canSetTeamColor()) {
            ctx.fail("cannot set the team colour (not a team leader, or no rgb)");
            return false;
        }
        team_game::setTeamColor(rgb->r, rgb->g, rgb->b);
        return true;
    }

    // team (default ours), rgb: the colour the server reports for that team.
    if (op == "expectTeamColor") {
        const auto it =
            team_game::teams().find(step.value("team", Session::instance().selfTeamId()));
        const team_game::Team* team = it == team_game::teams().end() ? nullptr : &it->second;
        const std::string got =
            team != nullptr && team->hasColor ?
                ui::color::formatHex({team->colorR, team->colorG, team->colorB}) :
                std::string("none");
        const std::string want = step.value("rgb", std::string{});
        return waitFor(
            ctx, got == want, 30.0, fmt::format("team colour is {}, want {}", got, want));
    }

    // owner: a peer name or "self".
    if (op == "expectRoomOwner") {
        const std::string owner = step.value("owner", std::string("self"));
        const Session& session = Session::instance();
        const Client* peer = owner == "self" ? nullptr : findPeer(owner);
        const uint32_t want =
            owner == "self" ? session.selfClientId() : (peer != nullptr ? peer->clientId : 0);
        const uint32_t got = session.roomState().ownerClientId;
        return waitFor(ctx, want != 0 && got == want, 60.0,
            fmt::format("room owner is client {}, want {}", got, owner));
    }

    // code: the refusal teleport::blockCode gives for peer, or null for none.
    if (op == "expectTeleportBlocked") {
        const std::string name = step.value("peer", std::string{});
        const Client* peer = findPeer(name);
        const json want = step.value("code", json());
        const char* got = peer != nullptr ? teleport::blockCode(peer->clientId) : "offline";
        const bool ok =
            want.is_null() ? got == nullptr : got != nullptr && want.get<std::string>() == got;
        return waitFor(ctx, ok, 30.0,
            fmt::format("teleport to {} blocked by {}", name, got != nullptr ? got : "nothing"));
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&teamGameSteps);

}  // namespace
}  // namespace twili::autotest
