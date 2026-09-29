#include "game/TeamGame.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "game/GameIdentity.hpp"
#include "sync/WorldSync.hpp"
#include "ui/StoryPrompt.hpp"
#include "ui/Toasts.hpp"

#if TWILI_ENABLE_AUTOTEST
#include "autotest/AutoTest.hpp"
#endif

#include <mods/svc/ui.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <chrono>

namespace twili::team_game {
namespace {

// Base64 has no break opportunities; the window wraps these chunks instead.
constexpr auto kTeamColorPushInterval = std::chrono::milliseconds(250);

struct Conflict {
    bool open = false;
    Game teamGame;
    int teammates = 0;
};

struct State {
    std::map<std::string, Team> teams;
    Sync localSync = Sync::Pending;
    bool knowOwnTeam = false;
    std::string ownGameKey;
    uint32_t ownOwner = 0;
    std::string toastedMismatch;
    Conflict conflict;
    // The picker's latest team colour, until the server reports it back.
    bool colorPending = false;
    bool colorDirty = false;
    uint8_t pendingColor[3] = {};
    std::chrono::steady_clock::time_point colorSentAt{};
    // Kept across reconnects: asked once per game session.
    bool unverifiedConfirmed = false;
    std::string askedUnverifiedKey;
};

State s_state;

Sync parseSync(const std::string& text) {
    if (text == "ok") {
        return Sync::Ok;
    }
    if (text == "unverified") {
        return Sync::Unverified;
    }
    if (text == "mismatch") {
        return Sync::Mismatch;
    }
    return Sync::Pending;
}

Game parseGame(const nlohmann::json& j) {
    Game g;
    if (!j.is_object()) {
        return g;
    }
    g.set = true;
    g.kind = j.value("kind", std::string{});
    g.key = j.value("key", std::string{});
    if (const auto display = j.find("display"); display != j.end() && display->is_object()) {
        g.name = display->value("name", std::string{});
        g.mode = display->value("mode", std::string{});
    }
    if (const auto share = j.find("share"); share != j.end() && share->is_object()) {
        g.permalink = share->value("permalink", std::string{});
        g.seed = share->value("seed", std::string{});
        g.version = share->value("version", std::string{});
    }
    return g;
}

std::string teamTitle(const std::string& id) {
    return id.empty() ? std::string("Players without a team") : "Team " + id;
}

// The team in a sentence: players without one share the room's game.
std::string teamRef(const std::string& id, bool capital) {
    if (!id.empty()) {
        return "Team " + id;
    }
    return capital ? "The room" : "the room";
}

std::string clientName(uint32_t id) {
    const auto& clients = Session::instance().clients();
    const auto it = clients.find(id);
    return it != clients.end() && !it->second.name.empty() ? it->second.name :
                                                             fmt::format("Player {}", id);
}

bool promptsSuppressed() {
#if TWILI_ENABLE_AUTOTEST
    return autotest::isActive();
#else
    return false;
#endif
}

std::string localLabel() {
    const game_identity::Identity& id = game_identity::current();
    if (!id.inGame) {
        return "not in game";
    }
    std::string label = id.mode.empty() ? id.kind : id.mode;
    if (!id.name.empty()) {
        label += ": " + id.name;
    }
    return label;
}

void onOwnTeam(const Team& team) {
    State& st = s_state;
    const uint32_t self = Session::instance().selfClientId();
    Sync now = Sync::Pending;
    for (const Member& m : team.members) {
        if (m.clientId == self) {
            now = m.sync;
        }
    }
    const Sync before = st.localSync;
    const bool first = !st.knowOwnTeam;
    const std::string title = teamTitle(team.id);
    const std::string gameKey = team.game.set ? team.game.key : std::string{};
    if (!first && !gameKey.empty() && !st.ownGameKey.empty() && gameKey != st.ownGameKey) {
        ui::toast("Twili-Together",
            fmt::format("{} now plays {}.", teamRef(team.id, true), gameLabel(team.game)));
    }
    if (!first && team.ownerClientId != 0 && team.ownerClientId != st.ownOwner) {
        ui::toast("Twili-Together",
            team.ownerClientId == self ?
                fmt::format("You now lead {}.", teamRef(team.id, false)) :
                fmt::format("{} now leads {}.", clientName(team.ownerClientId),
                    teamRef(team.id, false)));
    }
    if (gameKey != st.ownGameKey) {
        TwiliLog.info("[game] {} plays {}", title, gameKey.empty() ? "nothing yet" : gameKey);
    }
    st.ownGameKey = gameKey;
    st.ownOwner = team.ownerClientId;
    st.localSync = now;
    st.knowOwnTeam = true;
    if (now != before) {
        TwiliLog.info("[game] sync state {} -> {}", syncName(before), syncName(now));
    }
    if (now == Sync::Ok && before != Sync::Ok) {
        // Merge the team's progress into this save before our own goes out.
        sync::requestExchange();
        st.conflict.open = false;
        if (before == Sync::Mismatch || before == Sync::Unverified) {
            ui::toast("Twili-Together",
                fmt::format("Synced with {} ({}).", teamRef(team.id, false), gameLabel(team.game)));
        }
    }
    if (now == Sync::Mismatch) {
        const std::string key = gameKey + "|" + game_identity::current().key;
        if (key != st.toastedMismatch) {
            st.toastedMismatch = key;
            const std::string body =
                team.game.kind == "randomizer" && !team.game.name.empty() ?
                    fmt::format("World sync is off: {}'s seed is <b>{}</b>. Copy its permalink on "
                                "the Room tab to join.",
                        ui::escapeRml(teamRef(team.id, false)), ui::escapeRml(team.game.name)) :
                    fmt::format("World sync is off: {} plays <b>{}</b>.",
                        ui::escapeRml(teamRef(team.id, false)),
                        ui::escapeRml(gameLabel(team.game)));
            ui::toastInline("Twili-Together", body, ui::kToastWarning, 8000);
        }
    }
}

void handleTeamState(const nlohmann::json& p) {
    Team team;
    team.id = p.value("teamId", std::string{});
    team.ownerClientId = p.value("ownerClientId", 0u);
    if (const auto game = p.find("game"); game != p.end()) {
        team.game = parseGame(*game);
    }
    team.sameGameAsYours = p.value("sameGameAsYours", false);
    if (const auto color = p.find("color"); color != p.end() && color->is_object()) {
        team.hasColor = true;
        team.colorR = static_cast<uint8_t>(color->value("r", 255u));
        team.colorG = static_cast<uint8_t>(color->value("g", 255u));
        team.colorB = static_cast<uint8_t>(color->value("b", 255u));
    }
    if (const auto members = p.find("members"); members != p.end() && members->is_array()) {
        for (const auto& j : *members) {
            if (!j.is_object()) {
                continue;
            }
            Member m;
            m.clientId = j.value("clientId", 0u);
            m.sync = parseSync(j.value("sync", std::string{}));
            m.inGame = j.value("inGame", false);
            m.kind = j.value("kind", std::string{});
            m.name = j.value("name", std::string{});
            m.mode = j.value("mode", std::string{});
            team.members.push_back(std::move(m));
        }
    }
    State& st = s_state;
    const bool own = team.id == Session::instance().selfTeamId();
    if (team.members.empty()) {
        st.teams.erase(team.id);
    } else {
        st.teams[team.id] = team;
    }
    if (own) {
        const uint8_t* c = st.pendingColor;
        if (st.colorPending && !st.colorDirty && team.colorR == c[0] && team.colorG == c[1] &&
            team.colorB == c[2])
        {
            st.colorPending = false;
        }
        onOwnTeam(team);
    }
}

void handleConflict(const nlohmann::json& p) {
    State& st = s_state;
    st.conflict.open = true;
    st.conflict.teamGame = parseGame(p.value("game", nlohmann::json()));
    st.conflict.teammates = p.value("teammatesInGame", 0);
    TwiliLog.info("[game] {} teammate(s) still play {}; the team keeps it until you claim it",
        st.conflict.teammates, st.conflict.teamGame.key);
}

void showConflict() {
    State& st = s_state;
    st.conflict.open = false;
    const std::string body = fmt::format(
        "<p>{} teammate(s) still play <b>{}</b>.</p><p>Make your game (<b>{}</b>) the team's game? "
        "They stop syncing until they load it too.</p>",
        st.conflict.teammates, ui::escapeRml(gameLabel(st.conflict.teamGame)),
        ui::escapeRml(localLabel()));
    if (promptsSuppressed()) {
        TwiliLog.info("[game] team game conflict prompt (not shown under autotest)");
        return;
    }
    ui::showTeamPrompt({
        .title = "Switch the team's game?",
        .bodyRml = body,
        .acceptLabel = "Switch Team",
        .declineLabel = "Keep",
        .onAccept = [] { claimTeamGame(); },
    });
}

void showUnverifiedPrompt() {
    const Team* team = ownTeam();
    const std::string body = fmt::format(
        "<p>{} plays a randomizer game that can only be compared by probing a few item checks: no "
        "generated seed on this computer matches it.</p><p>If the seeds differ, world sync mixes "
        "two games into your save. Sync anyway for this session?</p>",
        ui::escapeRml(teamTitle(team != nullptr ? team->id : std::string{})));
    if (promptsSuppressed()) {
        TwiliLog.info("[game] unverified match prompt (not shown under autotest)");
        return;
    }
    ui::showTeamPrompt({
        .title = "Unverified randomizer game",
        .bodyRml = body,
        .acceptLabel = "Sync",
        .declineLabel = "Not Now",
        .onAccept = [] { allowUnverified(); },
    });
}

}  // namespace

const char* syncName(Sync sync) {
    switch (sync) {
    case Sync::Ok:
        return "ok";
    case Sync::Unverified:
        return "unverified";
    case Sync::Mismatch:
        return "mismatch";
    default:
        return "pending";
    }
}

bool handlePacket(const std::string& type, const nlohmann::json& packet) {
    if (type == "TEAM_STATE") {
        handleTeamState(packet);
    } else if (type == "TEAM_GAME_CONFLICT") {
        handleConflict(packet);
    } else {
        return false;
    }
    return true;
}

namespace {

void sendTeamColor() {
    State& st = s_state;
    const auto now = std::chrono::steady_clock::now();
    if (!st.colorDirty || now - st.colorSentAt < kTeamColorPushInterval || !canSetTeamColor()) {
        return;
    }
    st.colorDirty = false;
    st.colorSentAt = now;
    const uint8_t* c = st.pendingColor;
    Session::instance().send(
        {{"type", "SET_TEAM_COLOR"}, {"color", {{"r", c[0]}, {"g", c[1]}, {"b", c[2]}}}});
}

}  // namespace

void tick() {
    game_identity::tick();
    sendTeamColor();
    State& st = s_state;
    const std::string& key = game_identity::current().key;
    const bool askUnverified =
        st.localSync == Sync::Unverified && !unverifiedAllowed() && key != st.askedUnverifiedKey;
    if (!st.conflict.open && !askUnverified) {
        return;
    }
    // Never over another document; they wait until it closes.
    if (!promptsSuppressed() && (ui::teamPromptShowing() || ui::anyDocumentVisible())) {
        return;
    }
    if (st.conflict.open) {
        showConflict();
        return;
    }
    st.askedUnverifiedKey = key;
    showUnverifiedPrompt();
}

void resetSession() {
    State& st = s_state;
    const bool confirmed = st.unverifiedConfirmed;
    std::string asked = std::move(st.askedUnverifiedKey);
    st = State{};
    st.unverifiedConfirmed = confirmed;
    st.askedUnverifiedKey = std::move(asked);
    game_identity::resetSession();
}

const std::map<std::string, Team>& teams() {
    return s_state.teams;
}

const Team* ownTeam() {
    if (!Session::active()) {
        return nullptr;
    }
    const auto it = s_state.teams.find(Session::instance().selfTeamId());
    return it == s_state.teams.end() ? nullptr : &it->second;
}

const Member* member(uint32_t clientId) {
    for (const auto& [id, team] : s_state.teams) {
        for (const Member& m : team.members) {
            if (m.clientId == clientId) {
                return &m;
            }
        }
    }
    return nullptr;
}

Sync localSync() {
    return s_state.localSync;
}

bool memberMaySync(uint32_t clientId) {
    if (clientId == Session::instance().selfClientId()) {
        return s_state.localSync == Sync::Ok;
    }
    // Unknown members: the server only relays what may sync.
    const Member* m = member(clientId);
    return m == nullptr || m->sync == Sync::Ok;
}

bool isTeamOwner() {
    const Team* team = ownTeam();
    return team != nullptr && team->ownerClientId != 0 &&
           team->ownerClientId == Session::instance().selfClientId();
}

bool unverifiedAllowed() {
    return s_state.unverifiedConfirmed || config::getBool(config::Var::RandoAllowUnverified);
}

void allowUnverified() {
    if (!s_state.unverifiedConfirmed) {
        TwiliLog.info("[game] unverified randomizer match allowed for this session");
    }
    s_state.unverifiedConfirmed = true;
    game_identity::republish();
}

void claimTeamGame() {
    if (!isTeamOwner() || !Session::instance().isConnected()) {
        return;
    }
    TwiliLog.info("[game] claiming the team game for {}", game_identity::current().key);
    s_state.conflict.open = false;
    Session::instance().send({{"type", "CLAIM_TEAM_GAME"}});
}

bool conflictOpen() {
    return isTeamOwner() && s_state.localSync == Sync::Mismatch && game_identity::current().inGame;
}

const char* teleportBlock(uint32_t clientId) {
    const Session& session = Session::instance();
    const auto it = session.clients().find(clientId);
    if (it == session.clients().end() || it->second.teamId == session.selfTeamId()) {
        return nullptr;
    }
    if (!session.roomState().teleportAcrossTeams) {
        return "other-team";
    }
    const auto theirs = s_state.teams.find(it->second.teamId);
    if (theirs == s_state.teams.end() || !theirs->second.game.set ||
        !theirs->second.sameGameAsYours)
    {
        return "other-game";
    }
    return nullptr;
}

void promoteRoomOwner(uint32_t clientId) {
    Session& session = Session::instance();
    if (session.isRoomOwner() && clientId != session.selfClientId()) {
        TwiliLog.info("[game] handing the room to {}", clientName(clientId));
        session.send({{"type", "SET_ROOM_OWNER"}, {"targetClientId", clientId}});
    }
}

bool canSetTeamColor() {
    return Session::instance().isConnected() && !Session::instance().selfTeamId().empty() &&
           isTeamOwner();
}

void setTeamColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!canSetTeamColor()) {
        return;
    }
    State& st = s_state;
    st.pendingColor[0] = r;
    st.pendingColor[1] = g;
    st.pendingColor[2] = b;
    st.colorPending = true;
    st.colorDirty = true;
    sendTeamColor();
}

bool teamColor(uint8_t& r, uint8_t& g, uint8_t& b) {
    const State& st = s_state;
    if (st.colorPending) {
        r = st.pendingColor[0];
        g = st.pendingColor[1];
        b = st.pendingColor[2];
        return true;
    }
    const Team* team = ownTeam();
    if (team == nullptr || !team->hasColor) {
        return false;
    }
    r = team->colorR;
    g = team->colorG;
    b = team->colorB;
    return true;
}

void promoteTeamLeader(uint32_t clientId) {
    Session& session = Session::instance();
    if (isTeamOwner() && clientId != session.selfClientId()) {
        TwiliLog.info("[game] handing the team to {}", clientName(clientId));
        session.send({{"type", "SET_TEAM_OWNER"}, {"targetClientId", clientId}});
    }
}

std::string gameLabel(const Game& game) {
    if (!game.set) {
        return "no game yet";
    }
    if (game.kind == "vanilla") {
        return game.mode.empty() ? std::string("Vanilla") : game.mode;
    }
    std::string label = game.mode.empty() ? game.kind : game.mode;
    if (!game.name.empty()) {
        label += ": " + game.name;
    }
    return label;
}

std::string memberBadge(const Member& m) {
    switch (m.sync) {
    case Sync::Ok:
        return "synced";
    case Sync::Unverified:
        return "unverified match";
    case Sync::Mismatch:
        return "different game";
    default:
        return m.inGame ? "waiting for the team's game" : "not in game";
    }
}

// The seed's name for a randomizer game, the game's label otherwise.
std::string gameLineRml(const Game& game) {
    if (game.kind == "randomizer" && !game.name.empty()) {
        return "<p>Seed: <b>" + ui::escapeRml(game.name) + "</b></p>";
    }
    return "<p>Game: <b>" + ui::escapeRml(gameLabel(game)) + "</b></p>";
}

std::string statusRml() {
    const Session& session = Session::instance();
    if (!session.isConnected()) {
        return "Not connected. Players on the same team share progress while they play the same "
               "game; your team's game is set by its leader.";
    }
    const Team* team = ownTeam();
    const game_identity::Identity& local = game_identity::current();
    const std::string title = teamTitle(session.selfTeamId());
    std::string rml = "<p>" + ui::escapeRml(title);
    if (team != nullptr && team->ownerClientId != 0) {
        rml += ui::escapeRml(
            isTeamOwner() ? " · you lead it" : " · leader " + clientName(team->ownerClientId));
    }
    rml += "</p>";
    const Game* teamGame = team != nullptr && team->game.set ? &team->game : nullptr;
    if (teamGame != nullptr) {
        rml += gameLineRml(*teamGame);
    }
    if (s_state.localSync != Sync::Ok && local.inGame) {
        rml += "<p>Yours: " + ui::escapeRml(localLabel()) + (!local.verified ? " (unverified)" : "") +
               "</p>";
    }

    std::string why;
    switch (s_state.localSync) {
    case Sync::Ok:
        why = "World sync is on.";
        if (game_identity::isUnverifiedKey(local.key)) {
            why += " You allowed this unverified match.";
        }
        break;
    case Sync::Unverified:
        why = "Your seed matches the team's by item checks only. Press Sync Unverified Match to "
              "sync anyway.";
        break;
    case Sync::Mismatch:
        if (teamGame != nullptr && teamGame->kind == "randomizer") {
            why = "World sync is off: copy the team's permalink to start its seed.";
        } else if (teamGame != nullptr && teamGame->kind == "vanilla" && local.kind == "randomizer")
        {
            why = "World sync is off: load a vanilla save to sync.";
        } else {
            why = "World sync is off: your game differs from the team's.";
        }
        break;
    default:
        why = !local.inGame        ? "Load a save to sync with your team." :
              teamGame == nullptr ? "The first player in game sets the team's game." :
                                    "Waiting for the server.";
        break;
    }
    rml += "<p>" + ui::escapeRml(why) + "</p>";
    return rml;
}

std::string permalinkRml() {
    const Team* team = ownTeam();
    if (team == nullptr || !team->game.set || team->game.kind != "randomizer" ||
        !team->game.permalink.empty())
    {
        return {};
    }
    return "<p>The leader's seed has no permalink to share.</p>";
}

const std::string& ownPermalink() {
    static const std::string kNone;
    const Team* team = ownTeam();
    return team != nullptr ? team->game.permalink : kNone;
}

bool copyPermalink() {
    const std::string& permalink = ownPermalink();
    if (permalink.empty() || svc_ui->set_clipboard_text(mod_ctx, permalink.c_str()) != MOD_OK) {
        return false;
    }
    ui::toast("Twili-Together", "Permalink copied.");
    return true;
}

std::string panelLine() {
    const Session& session = Session::instance();
    const Team* team = ownTeam();
    if (!session.isConnected() || team == nullptr) {
        return {};
    }
    const char* state = s_state.localSync == Sync::Ok         ? "synced" :
                        s_state.localSync == Sync::Unverified ? "unverified" :
                        s_state.localSync == Sync::Mismatch   ? "different game" :
                                                                "not syncing";
    return fmt::format("Team game: {} ({})", gameLabel(team->game), state);
}

}  // namespace twili::team_game
