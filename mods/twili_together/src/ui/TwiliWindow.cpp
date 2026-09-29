#include "ui/TwiliWindow.hpp"

#include "core/Config.hpp"
#include "core/Layout.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "game/TeamGame.hpp"
#include "story/StoryUi.hpp"
#include "teleport/Teleport.hpp"
#include "ui/ColorMath.hpp"
#include "ui/StoryPrompt.hpp"
#include "ui/Toasts.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace twili::ui {
namespace {

using config::Var;

// Distinct as tunic tints and on the dark name-tag fill; white is the default colour.
constexpr const char* kColorPresets[] = {
    "e53935", "fb8c00", "fdd835", "7cb342", "2e9d4a", "00897b",
    "00acc1", "1e88e5", "5e35b1", "8e24aa", "d81b60", "ffffff",
};
constexpr color::Rgb8 kDefaultColor{255, 255, 255};
constexpr size_t kTeleportSlots = 12;

UiMenuTabHandle s_menuTab = 0;
UiWindowHandle s_window = 0;

// Elements of the tab on screen; a tab switch destroys them.
struct Live {
    UiElementHandle status = 0;
    UiElementHandle color = 0;
    UiElementHandle tagPreview = 0;
    UiElementHandle roomInfo = 0;
    UiElementHandle teamInfo = 0;
    UiElementHandle teamPermalink = 0;
    UiElementHandle teamColor = 0;
    bool teamColorShown = false;
    UiElementHandle players = 0;
    std::array<UiElementHandle, kTeleportSlots> teleport{};
    std::array<uint32_t, kTeleportSlots> teleportIds{};
    UiElementHandle teleportStatus = 0;
    // [0] Make ... Room Owner, [1] Make ... Team Leader.
    std::array<std::array<UiElementHandle, kTeleportSlots>, 2> promote{};
    std::array<std::array<uint32_t, kTeleportSlots>, 2> promoteIds{};
    std::array<std::array<std::string, kTeleportSlots>, 2> lastPromoteLabels;
    UiElementHandle storyStatus = 0;
    UiElementHandle catchUp = 0;
    std::string lastStatus;
    std::string lastTag;
    std::string lastRoomInfo;
    std::string lastTeamInfo;
    std::string lastTeamPermalink;
    std::string lastPlayers;
    std::array<std::string, kTeleportSlots> lastTeleportLabels;
    std::string lastTeleportStatus;
    std::string lastStoryStatus;
    std::string lastCatchUp;
};
Live s_live;

// The Mods window panel.
UiElementHandle s_panelStatus = 0;
std::string s_panelLastStatus;
UiElementHandle s_panelTeam = 0;
std::string s_panelLastTeam;

void addControl(UiElementHandle pane, UiControlDesc& control, UiElementHandle* out = nullptr) {
    if (svc_ui->pane_add_control(mod_ctx, pane, &control, out) != MOD_OK) {
        TwiliLog.warn("[ui] could not add control '{}'", control.label);
    }
}

UiElementHandle addRml(UiElementHandle pane, const std::string& rml) {
    UiElementHandle elem = 0;
    svc_ui->pane_add_rml(mod_ctx, pane, rml.c_str(), &elem);
    return elem;
}

void setRml(UiElementHandle elem, std::string& last, std::string rml) {
    if (elem != 0 && rml != last) {
        last = std::move(rml);
        svc_ui->elem_set_rml(mod_ctx, elem, last.c_str());
    }
}

void setText(UiElementHandle elem, std::string& last, std::string text) {
    if (elem != 0 && text != last) {
        last = std::move(text);
        svc_ui->elem_set_text(mod_ctx, elem, last.c_str());
    }
}

void setLabel(UiElementHandle control, std::string& last, std::string label) {
    if (control != 0 && label != last) {
        last = std::move(label);
        svc_ui->control_set_label(mod_ctx, control, last.c_str());
    }
}

std::string colorCss(uint8_t r, uint8_t g, uint8_t b) {
    return fmt::format("#{:02X}{:02X}{:02X}", r, g, b);
}

color::Rgb8 settingColor() {
    const auto c = localPlayerColor();
    return {static_cast<uint8_t>(c[0]), static_cast<uint8_t>(c[1]), static_cast<uint8_t>(c[2])};
}

// --- Connection tab

void addString(UiElementHandle pane, Var var, const char* label, const char* help,
    int32_t maxLength) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_STRING;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = config::handle(var);
    control.max_length = maxLength;
    addControl(pane, control);
}

void addToggle(UiElementHandle pane, Var var, const char* label, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = config::handle(var);
    addControl(pane, control);
}

// The picker writes "rrggbb"; its Default button writes "", which means white here.
void getColor(ModContext*, void*, UiControlValue* out) {
    static std::string s_value;
    s_value = color::formatConfig(settingColor());
    out->string_value = s_value.c_str();
}

void setColor(ModContext*, void*, const UiControlValue* value) {
    const auto parsed = color::parseHex(value->string_value != nullptr ? value->string_value : "");
    const std::string text = color::formatConfig(parsed.value_or(kDefaultColor));
    if (text != color::formatConfig(settingColor())) {
        config::setString(Var::Color, text);
    }
}

bool colorModified(ModContext*, void*) {
    return settingColor() != kDefaultColor;
}

// How the others see us: the name tag's frame and dot take the colour.
std::string tagPreviewRml() {
    const color::Rgb8 c = settingColor();
    const std::string name = config::getString(Var::DisplayName);
    const std::string css = colorCss(c.r, c.g, c.b);
    return fmt::format("<div class=\"twili-tag\" style=\"border-color: {0};\">"
                       "<span class=\"twili-tag-dot\" style=\"background-color: {0};\"></span>"
                       "<span class=\"twili-tag-name\">{1}</span></div>",
        css, escapeRml(name.empty() ? "Player" : name));
}

bool isIdle(ModContext*, void*) {
    return Session::instance().state() == SessionState::Disconnected;
}

bool isNotIdle(ModContext* ctx, void* data) {
    return !isIdle(ctx, data);
}

void onConnect(ModContext*, void*) {
    Session::instance().connect();
}

void onDisconnect(ModContext*, void*) {
    Session::instance().disconnect();
}

ModResult buildConnectionTab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle,
    void*, ModError*) {
    s_live = {};
    svc_ui->pane_add_section(mod_ctx, left, "Identity");
    addString(left, Var::DisplayName, "Name",
        "<p>Your name on your name tag. Takes effect on the next connect.</p>", 20);
    addString(left, Var::TeamId, "Team",
        "<p>Players on the same team share flags, items and save progress while they play the "
        "same game, and show in the team's colour. Other teams stay visible. Takes effect on the "
        "next connect.</p>",
        16);
    addString(left, Var::RoomId, "Room",
        "<p>Only players with the same room code on the same server see each other. Empty is "
        "the server's public room. Takes effect on the next connect.</p>",
        32);

    svc_ui->pane_add_section(mod_ctx, left, "Player Colour");
    UiControlDesc color = UI_CONTROL_DESC_INIT;
    color.kind = UI_CONTROL_COLOR;
    color.label = "Colour";
    color.help_rml = "<p>The colour other players see on your name tag, map marker and outfit. "
                     "Changes reach everyone in the room at once.</p>";
    color.binding = UI_BINDING_CALLBACKS;
    color.get = getColor;
    color.set = setColor;
    color.is_modified = colorModified;
    color.color_presets = kColorPresets;
    color.color_preset_count = std::size(kColorPresets);
    addControl(left, color, &s_live.color);
    s_live.lastTag = tagPreviewRml();
    s_live.tagPreview = addRml(left, s_live.lastTag);

    svc_ui->pane_add_section(mod_ctx, left, "Notifications");
    addToggle(left, Var::ItemToasts, "Item Pickups",
        "<p>A notification with the item's icon when a teammate picks up an item, key, map or "
        "heart piece. Items synced when you join are summarized.</p>");
    addToggle(left, Var::ItemToastsOwn, "Your Own Pickups",
        "<p>Also notify for items you pick up yourself.</p>");
    addToggle(left, Var::StoryPrompts, "Story Prompts",
        "<p>Ask whether to follow when a teammate's story event moves them somewhere else.</p>");

    svc_ui->pane_add_section(mod_ctx, left, "Server");
    addString(left, Var::ServerUrl, "Server URL",
        "<p>wss://host for internet play (the relay needs a real certificate).</p>"
        "<p>tcp://host:port for LAN play: unencrypted.</p>"
        "<p>ws://localhost:3000 for a relay on this computer.</p>",
        256);
    addToggle(left, Var::AutoReconnect, "Reconnect Automatically",
        "<p>After a lost connection, connect again with growing pauses.</p>");
    s_live.lastStatus = Session::instance().statusText();
    svc_ui->pane_add_text(mod_ctx, left, s_live.lastStatus.c_str(), &s_live.status);

    UiControlDesc connect = UI_CONTROL_DESC_INIT;
    connect.kind = UI_CONTROL_BUTTON;
    connect.label = "Connect";
    connect.on_pressed = onConnect;
    connect.is_disabled = isNotIdle;
    addControl(left, connect);

    UiControlDesc disconnect = UI_CONTROL_DESC_INIT;
    disconnect.kind = UI_CONTROL_BUTTON;
    disconnect.label = "Disconnect";
    disconnect.on_pressed = onDisconnect;
    disconnect.is_disabled = isIdle;
    addControl(left, disconnect);
    return MOD_OK;
}

ModResult updateConnectionTab(ModContext*, void*, ModError*) {
    setText(s_live.status, s_live.lastStatus, Session::instance().statusText());
    setRml(s_live.tagPreview, s_live.lastTag, tagPreviewRml());
    return MOD_OK;
}

// --- Room tab

// A non-owner sees the room's values, read-only; otherwise the controls edit our defaults.
bool roomLocked() {
    const Session& session = Session::instance();
    return session.isConnected() && !session.isRoomOwner();
}

bool roomLockedPredicate(ModContext*, void*) {
    return roomLocked();
}

struct RoomBool {
    Var var;
    bool RoomState::*field;
    const char* label;
    const char* help;
    // Greyed out while this setting is off.
    Var needs = Var::Count;
};

struct RoomInt {
    Var var;
    int RoomState::*field;
    const char* label;
    const char* help;
};

const RoomBool kSyncOptions[] = {
    {Var::SyncWorldState, &RoomState::syncWorldState, "Sync World State",
        "<p>Share flags, items, dungeon keys and save progress with players on the same "
        "team.</p>"},
    {Var::ShareWoodenShield, &RoomState::shareWoodenShield, "Share Wooden Shields",
        "<p>On: wooden shields sync like other gear, so a shield that burns up comes back while a "
        "teammate still has one.</p><p>Off: the Ordon Shield and Wooden Shield are personal. A "
        "burnt shield stays burnt and each player gets their own.</p>"},
    {Var::SyncEnemyDeaths, &RoomState::syncNPCs, "Sync Enemy Deaths",
        "<p>When a teammate in your area defeats a regular enemy, it disappears for you too. "
        "Bosses and mini-bosses are excluded, and drops go to whoever defeated it.</p>"},
    {Var::SyncEnemyDamage, &RoomState::syncEnemyDamage, "Share Enemy Damage",
        "<p>Teammates wear down the same enemies: your hits weaken their copy too. The player who "
        "lands the last blow gets the drop. Enemies go down faster with more players; consider "
        "raising Enemy Health. Needs Sync Enemy Deaths.</p>",
        Var::SyncEnemyDeaths},
    {Var::CutsceneSync, &RoomState::cutsceneSync, "Cutscene Sync",
        "<p>When a story cutscene starts for a teammate, players in the same room who are "
        "standing safely watch it too. Needs Sync World State.</p>"},
    {Var::HidePlayersInCutscene, &RoomState::hidePlayersInCutscene, "Hide Players in Cutscenes",
        "<p>Hide other players, their name tags and map markers while a cutscene plays for you, "
        "and hide you from them during theirs. Conversations don't count.</p>"},
};

const RoomBool kPvpOptions[] = {
    {Var::PvpMode, &RoomState::pvpMode, "PvP Mode",
        "<p>Let players hurt each other. The attacker's game decides whether a hit lands; a "
        "raised shield still blocks it, and nothing lands during invincibility after a hit, "
        "events or cutscenes.</p>"},
    {Var::PvpFriendlyFire, &RoomState::pvpFriendlyFire, "Friendly Fire",
        "<p>Players on the same team can hurt each other too. Players without a team can always "
        "be hurt while PvP is on.</p>"},
    {Var::PvpLethal, &RoomState::pvpLethal, "Lethal PvP",
        "<p>Other players can knock you out. When off, their hits never take you below one "
        "heart.</p>"},
};

const RoomBool kVisibilityOptions[] = {
    {Var::ShowLocations, &RoomState::showLocationsMode, "Show Locations",
        "<p>Show other players' positions on the minimap and the dungeon map.</p>"},
    {Var::TeleportMode, &RoomState::teleportMode, "Teleport to Player",
        "<p>Allow players to teleport to each other from the Players tab.</p>"},
    {Var::TeleportAcrossTeams, &RoomState::teleportAcrossTeams, "Teleport Across Teams",
        "<p>Allow teleporting to players of other teams. Teams playing different games (vanilla, "
        "different randomizer seeds) never teleport to each other.</p>"},
};

const RoomInt kDifficultyOptions[] = {
    {Var::EnemyHealthMultiplier, &RoomState::enemyHealthMultiplier, "Enemy Health",
        "<p>Health of regular enemies. 100% is normal. Bosses, mini-bosses and enemies that "
        "always die in one hit keep normal health.</p>"},
};

void getRoomBool(ModContext*, void* data, UiControlValue* out) {
    const auto* opt = static_cast<const RoomBool*>(data);
    out->bool_value =
        roomLocked() ? Session::instance().roomState().*(opt->field) : config::getBool(opt->var);
}

void setRoomBool(ModContext*, void* data, const UiControlValue* value) {
    const auto* opt = static_cast<const RoomBool*>(data);
    if (!roomLocked() && value->bool_value != config::getBool(opt->var)) {
        config::setBool(opt->var, value->bool_value);
    }
}

bool roomBoolDisabled(ModContext*, void* data) {
    const auto* opt = static_cast<const RoomBool*>(data);
    return roomLocked() || (opt->needs != Var::Count && !config::getBool(opt->needs));
}

bool roomBoolModified(ModContext*, void* data) {
    const auto* opt = static_cast<const RoomBool*>(data);
    return config::getBool(opt->var) != RoomState{}.*(opt->field);
}

void getRoomInt(ModContext*, void* data, UiControlValue* out) {
    const auto* opt = static_cast<const RoomInt*>(data);
    out->int_value = roomLocked() ? Session::instance().roomState().*(opt->field)
                                  : config::getInt(opt->var);
}

void setRoomInt(ModContext*, void* data, const UiControlValue* value) {
    const auto* opt = static_cast<const RoomInt*>(data);
    if (!roomLocked()) {
        config::setInt(opt->var, std::clamp<int64_t>(value->int_value, 100, 500));
    }
}

bool roomIntModified(ModContext*, void* data) {
    const auto* opt = static_cast<const RoomInt*>(data);
    return config::getInt(opt->var) != RoomState{}.*(opt->field);
}

void addRoomBools(UiElementHandle pane, const char* section, const RoomBool* opts, size_t count) {
    svc_ui->pane_add_section(mod_ctx, pane, section);
    for (size_t i = 0; i < count; ++i) {
        UiControlDesc control = UI_CONTROL_DESC_INIT;
        control.kind = UI_CONTROL_TOGGLE;
        control.label = opts[i].label;
        control.help_rml = opts[i].help;
        control.get = getRoomBool;
        control.set = setRoomBool;
        control.is_disabled = roomBoolDisabled;
        control.is_modified = roomBoolModified;
        control.user_data = const_cast<RoomBool*>(&opts[i]);
        addControl(pane, control);
    }
}

std::string clientName(uint32_t id) {
    const auto& clients = Session::instance().clients();
    const auto it = clients.find(id);
    return it != clients.end() && !it->second.name.empty() ? it->second.name
                                                           : fmt::format("#{}", id);
}

std::string roomInfoRml() {
    const Session& session = Session::instance();
    if (!session.isConnected()) {
        return "Not connected. These values become the room settings when you own a room.";
    }
    if (session.isRoomOwner()) {
        return "You own this room: changes apply to everyone in it.";
    }
    const uint32_t ownerId = session.roomState().ownerClientId;
    if (ownerId == 0) {
        return "Joining the room...";
    }
    return "Room settings are controlled by " + escapeRml(clientName(ownerId)) + ".";
}

// --- Your team (Room tab)

void onCopyPermalink(ModContext*, void*) {
    team_game::copyPermalink();
}

bool noPermalink(ModContext*, void*) {
    return team_game::ownPermalink().empty();
}

void onClaimTeamGame(ModContext*, void*) {
    team_game::claimTeamGame();
}

bool cannotClaim(ModContext*, void*) {
    return !team_game::conflictOpen();
}

void onAllowUnverified(ModContext*, void*) {
    team_game::allowUnverified();
}

// The team leader's picker: the team colour everyone shows the team's players in.
void getTeamColor(ModContext*, void*, UiControlValue* out) {
    static std::string s_value;
    color::Rgb8 c;
    s_value = team_game::teamColor(c.r, c.g, c.b) ? color::formatConfig(c) : std::string{};
    out->string_value = s_value.c_str();
}

void setTeamColor(ModContext*, void*, const UiControlValue* value) {
    if (const auto parsed =
            color::parseHex(value->string_value != nullptr ? value->string_value : ""))
    {
        team_game::setTeamColor(parsed->r, parsed->g, parsed->b);
    }
}

bool cannotSetTeamColor(ModContext*, void*) {
    return !team_game::canSetTeamColor();
}

bool nothingToConfirm(ModContext*, void*) {
    return team_game::localSync() != team_game::Sync::Unverified || team_game::unverifiedAllowed();
}

// Hidden while empty.
void setOptionalRml(UiElementHandle elem, std::string& last, const std::string& rml) {
    const std::string shown = rml.empty() ? std::string(" ") : rml;
    if (elem != 0 && shown != last) {
        svc_ui->elem_set_visible(mod_ctx, elem, !rml.empty());
        setRml(elem, last, shown);
    }
}

void addTeamSection(UiElementHandle left) {
    svc_ui->pane_add_section(mod_ctx, left, "Your Team");
    s_live.lastTeamInfo = team_game::statusRml();
    s_live.teamInfo = addRml(left, s_live.lastTeamInfo);
    const std::string permalink = team_game::permalinkRml();
    s_live.lastTeamPermalink = permalink.empty() ? std::string(" ") : permalink;
    s_live.teamPermalink = addRml(left, s_live.lastTeamPermalink);
    if (s_live.teamPermalink != 0 && permalink.empty()) {
        svc_ui->elem_set_visible(mod_ctx, s_live.teamPermalink, false);
    }

    UiControlDesc copy = UI_CONTROL_DESC_INIT;
    copy.kind = UI_CONTROL_BUTTON;
    copy.label = "Copy Permalink";
    copy.help_rml = "<p>Copy your team's randomizer seed permalink, to paste it in the "
                    "Randomizer tab and generate the same seed.</p>";
    copy.on_pressed = onCopyPermalink;
    copy.is_disabled = noPermalink;
    addControl(left, copy);

    UiControlDesc claim = UI_CONTROL_DESC_INIT;
    claim.kind = UI_CONTROL_BUTTON;
    claim.label = "Make My Game the Team's Game";
    claim.help_rml = "<p>Team leader only: switch the team to the game you are playing. Teammates "
                     "stop syncing until they load it too.</p>";
    claim.on_pressed = onClaimTeamGame;
    claim.is_disabled = cannotClaim;
    addControl(left, claim);

    UiControlDesc confirm = UI_CONTROL_DESC_INIT;
    confirm.kind = UI_CONTROL_BUTTON;
    confirm.label = "Sync Unverified Match";
    confirm.help_rml =
        "<p>Your randomizer game matches the team's by item probes only. Sync anyway "
        "for this session.</p>";
    confirm.on_pressed = onAllowUnverified;
    confirm.is_disabled = nothingToConfirm;
    addControl(left, confirm);

    UiControlDesc teamColor = UI_CONTROL_DESC_INIT;
    teamColor.kind = UI_CONTROL_COLOR;
    teamColor.label = "Team Colour";
    teamColor.help_rml = "<p>Team leader only: everyone sees your team's players in this colour "
                         "(tunic, name tag, map marker). Players without a team keep theirs.</p>";
    teamColor.binding = UI_BINDING_CALLBACKS;
    teamColor.get = getTeamColor;
    teamColor.set = setTeamColor;
    teamColor.is_disabled = cannotSetTeamColor;
    teamColor.color_presets = kColorPresets;
    teamColor.color_preset_count = std::size(kColorPresets);
    addControl(left, teamColor, &s_live.teamColor);
    s_live.teamColorShown = team_game::canSetTeamColor();
    if (s_live.teamColor != 0 && !s_live.teamColorShown) {
        svc_ui->elem_set_visible(mod_ctx, s_live.teamColor, false);
    }

    addToggle(left, Var::RandoAllowUnverified, "Always Sync Unverified Randomizer Games",
        "<p>Sync with teammates whose randomizer game can only be compared by probing item checks "
        "(no generated seed here matches it), without asking.</p>");
}

ModResult buildRoomTab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle,
    void*, ModError*) {
    s_live = {};
    addTeamSection(left);
    svc_ui->pane_add_section(mod_ctx, left, "Room");
    s_live.lastRoomInfo = roomInfoRml();
    s_live.roomInfo = addRml(left, s_live.lastRoomInfo);
    addRoomBools(left, "Synchronization", kSyncOptions, std::size(kSyncOptions));
    addRoomBools(left, "PvP", kPvpOptions, std::size(kPvpOptions));
    addRoomBools(left, "Visibility", kVisibilityOptions, std::size(kVisibilityOptions));
    svc_ui->pane_add_section(mod_ctx, left, "Difficulty");
    for (const RoomInt& opt : kDifficultyOptions) {
        UiControlDesc control = UI_CONTROL_DESC_INIT;
        control.kind = UI_CONTROL_NUMBER;
        control.label = opt.label;
        control.help_rml = opt.help;
        control.get = getRoomInt;
        control.set = setRoomInt;
        control.is_disabled = roomLockedPredicate;
        control.is_modified = roomIntModified;
        control.user_data = const_cast<RoomInt*>(&opt);
        control.min = 100;
        control.max = 500;
        control.step = 25;
        control.suffix = "%";
        addControl(left, control);
    }
    return MOD_OK;
}

ModResult updateRoomTab(ModContext*, void*, ModError*) {
    setRml(s_live.roomInfo, s_live.lastRoomInfo, roomInfoRml());
    setRml(s_live.teamInfo, s_live.lastTeamInfo, team_game::statusRml());
    setOptionalRml(s_live.teamPermalink, s_live.lastTeamPermalink, team_game::permalinkRml());
    if (const bool leader = team_game::canSetTeamColor();
        s_live.teamColor != 0 && leader != s_live.teamColorShown)
    {
        s_live.teamColorShown = leader;
        svc_ui->elem_set_visible(mod_ctx, s_live.teamColor, leader);
    }
    return MOD_OK;
}

// --- Players tab

const team_game::Team* findTeam(const std::string& teamId) {
    const auto it = team_game::teams().find(teamId);
    return it == team_game::teams().end() ? nullptr : &it->second;
}

// "Team red · Randomizer 1.0.5: Soldier Beth Dragonfly"; other teams show the mode and seed name.
std::string teamHeaderRml(const std::string& teamId) {
    std::string text = teamId.empty() ? std::string("No team") : "Team " + teamId;
    std::string swatch;
    if (const team_game::Team* team = findTeam(teamId)) {
        text += " · " + team_game::gameLabel(team->game);
        if (team->sameGameAsYours) {
            text += " (same game as your team)";
        }
        if (team->hasColor) {
            swatch = fmt::format(
                "<span class=\"twili-swatch\" style=\"background-color: {};\"></span> ",
                colorCss(team->colorR, team->colorG, team->colorB));
        }
    }
    return "<p>" + swatch + "<b>" + escapeRml(text) + "</b></p>";
}

std::string playerRml(uint32_t id, const Client& c, bool ownTeam) {
    const Session& session = Session::instance();
    std::string tags;
    if (c.self) tags += " (you)";
    if (id == session.roomState().ownerClientId) tags += " [room owner]";
    if (const team_game::Team* team = findTeam(c.teamId);
        team != nullptr && team->ownerClientId == id)
    {
        tags += " [team leader]";
    }
    if (!c.online) tags += " [offline]";
    if (!c.self && c.protocolVersion != Session::kProtocolVersion) {
        tags += " [version mismatch]";
    } else if (!c.self && !c.layout.empty() && c.layout != layout::saveLayoutHex()) {
        tags += " [incompatible version]";
    }
    const std::string where =
        !c.isSaveLoaded ? "no save loaded"
                        : fmt::format("{} layer {} room {}",
                              std::string(c.stageName, strnlen(c.stageName, sizeof(c.stageName))),
                              c.layerNo, c.roomNo);
    std::string rml = fmt::format(
        "<p><span class=\"twili-swatch\" style=\"background-color: {};\"></span> {}{}</p>",
        colorCss(c.colorR, c.colorG, c.colorB), escapeRml(clientName(id)), escapeRml(tags));
    std::string details = where;
    // The form comes with PLAYER_UPDATE, which only players in our stage and layer send.
    if (c.isSaveLoaded && !c.self && c.hasPlayerUpdate) {
        details += c.transformStatus != 0 ? " (wolf)" : " (human)";
    }
    // Teammates: may they sync with us; other teams: what they play.
    if (const team_game::Member* m = team_game::member(id)) {
        if (ownTeam) {
            details += " · " + team_game::memberBadge(*m);
        } else if (m->inGame) {
            details += " · " + (m->mode.empty() ? m->kind : m->mode) +
                       (m->name.empty() ? "" : ": " + m->name);
        }
    }
    if (!c.modVersion.empty()) {
        details += " · " + c.modVersion;
    }
    rml += "<p class=\"twili-player-detail\">" + escapeRml(details) + "</p>";
    if (const std::string story = story::clientLine(id); !story.empty()) {
        rml += "<p class=\"twili-player-detail\">Story: " + escapeRml(story) + "</p>";
    }
    return rml;
}

// Grouped by team, ours first.
std::string playersRml() {
    const Session& session = Session::instance();
    if (!session.isConnected()) {
        return "Not connected.";
    }
    std::map<std::string, std::vector<uint32_t>> others;
    std::vector<uint32_t> ours;
    for (const auto& [id, c] : session.clients()) {
        (c.teamId == session.selfTeamId() ? ours : others[c.teamId]).push_back(id);
    }
    std::string rml;
    const auto addGroup = [&](const std::string& teamId, const std::vector<uint32_t>& ids) {
        if (ids.empty()) {
            return;
        }
        rml += teamHeaderRml(teamId);
        for (const uint32_t id : ids) {
            rml += playerRml(id, session.clients().at(id), teamId == session.selfTeamId());
        }
    };
    addGroup(session.selfTeamId(), ours);
    for (const auto& [teamId, ids] : others) {
        addGroup(teamId, ids);
    }
    return rml.empty() ? std::string("No players.") : rml;
}

std::vector<uint32_t> teleportTargets() {
    std::vector<uint32_t> ids;
    const Session& session = Session::instance();
    if (!session.isConnected()) {
        return ids;
    }
    for (const auto& [id, c] : session.clients()) {
        if (!c.self && c.online && ids.size() < kTeleportSlots) {
            ids.push_back(id);
        }
    }
    return ids;
}

std::string teleportLabel(uint32_t id) {
    const char* code = teleport::blockCode(id);
    const std::string name = clientName(id);
    return code == nullptr ? "Teleport to " + name
                           : fmt::format("Teleport to {} ({})", name, teleport::reasonText(code));
}

uint32_t slotClient(void* data) {
    const auto slot = reinterpret_cast<uintptr_t>(data);
    return slot < kTeleportSlots ? s_live.teleportIds[slot] : 0;
}

bool teleportDisabled(ModContext*, void* data) {
    const uint32_t id = slotClient(data);
    return id == 0 || teleport::blockCode(id) != nullptr;
}

void onTeleport(ModContext*, void* data) {
    if (const uint32_t id = slotClient(data); id != 0) {
        teleport::request(id);
    }
}

// --- handing over the room or the team

enum PromoteRole : uintptr_t { kRoomOwner = 0, kTeamLeader = 1 };

// Every other member for the room owner; every other teammate for the team leader.
std::vector<uint32_t> promoteTargets(PromoteRole role) {
    std::vector<uint32_t> ids;
    const Session& session = Session::instance();
    const bool allowed = role == kRoomOwner ? session.isRoomOwner() : team_game::isTeamOwner();
    if (!session.isConnected() || !allowed) {
        return ids;
    }
    for (const auto& [id, c] : session.clients()) {
        if (!c.self && c.online && ids.size() < kTeleportSlots &&
            (role == kRoomOwner || c.teamId == session.selfTeamId()))
        {
            ids.push_back(id);
        }
    }
    return ids;
}

void* promoteData(PromoteRole role, size_t slot) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(role) * kTeleportSlots + slot);
}

void onPromote(ModContext*, void* data) {
    const auto value = reinterpret_cast<uintptr_t>(data);
    const auto role = static_cast<PromoteRole>(value / kTeleportSlots);
    const uint32_t id = s_live.promoteIds[role][value % kTeleportSlots];
    if (id == 0) {
        return;
    }
    const std::string name = escapeRml(clientName(id));
    if (role == kRoomOwner) {
        pushStoryConfirm({
            .title = "Hand over the room",
            .bodyRml = "<p>Make <b>" + name +
                       "</b> the room owner? They take over the room settings, and you cannot "
                       "take the room back yourself.</p>",
            .acceptLabel = "Make Room Owner",
            .declineLabel = "Cancel",
            .onAccept = [id] { team_game::promoteRoomOwner(id); },
        });
    } else {
        pushStoryConfirm({
            .title = "Hand over the team",
            .bodyRml = "<p>Make <b>" + name +
                       "</b> the team leader? They decide the team's game from now on.</p>",
            .acceptLabel = "Make Team Leader",
            .declineLabel = "Cancel",
            .onAccept = [id] { team_game::promoteTeamLeader(id); },
        });
    }
}

std::string catchUpLabel() {
    const char* code = story::catchUpBlockCode();
    if (code == nullptr || std::string_view(code) == "nothing") {
        return story::catchUpTitle();
    }
    return fmt::format("{} ({})", story::catchUpTitle(), teleport::reasonText(code));
}

bool catchUpDisabled(ModContext*, void*) {
    return story::catchUpBlockCode() != nullptr;
}

void onCatchUp(ModContext*, void*) {
    if (story::catchUpBlockCode() != nullptr) {
        return;
    }
    pushStoryConfirm({
        .title = "Catch up to story",
        .bodyRml = story::catchUpConfirmRml(),
        .acceptLabel = "Go",
        .declineLabel = "Cancel",
        .onAccept = [] { story::startCatchUp(); },
    });
}

ModResult buildPlayersTab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle,
    void*, ModError*) {
    s_live = {};
    svc_ui->pane_add_section(mod_ctx, left, "Connected Players");
    s_live.lastPlayers = playersRml();
    s_live.players = addRml(left, s_live.lastPlayers);

    svc_ui->pane_add_section(mod_ctx, left, "Teleport");
    for (size_t i = 0; i < kTeleportSlots; ++i) {
        UiControlDesc button = UI_CONTROL_DESC_INIT;
        button.kind = UI_CONTROL_BUTTON;
        button.label = "Teleport";
        button.help_rml = "<p>Teleport to this player's last spot on solid ground. The room owner "
                          "turns teleporting on in the Room tab.</p>";
        button.on_pressed = onTeleport;
        button.is_disabled = teleportDisabled;
        button.user_data = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
        addControl(left, button, &s_live.teleport[i]);
        if (s_live.teleport[i] != 0) {
            svc_ui->elem_set_visible(mod_ctx, s_live.teleport[i], false);
        }
    }
    svc_ui->pane_add_text(mod_ctx, left, "", &s_live.teleportStatus);

    svc_ui->pane_add_section(mod_ctx, left, "Room Owner and Team Leader");
    svc_ui->pane_add_text(mod_ctx, left,
        "The room owner can hand the room to anyone here, a team leader the team to a teammate. "
        "When one of them leaves, the longest-connected player takes over.",
        nullptr);
    for (const PromoteRole role : {kRoomOwner, kTeamLeader}) {
        for (size_t i = 0; i < kTeleportSlots; ++i) {
            UiControlDesc button = UI_CONTROL_DESC_INIT;
            button.kind = UI_CONTROL_BUTTON;
            button.label = role == kRoomOwner ? "Make Room Owner" : "Make Team Leader";
            button.on_pressed = onPromote;
            button.user_data = promoteData(role, i);
            addControl(left, button, &s_live.promote[role][i]);
            if (s_live.promote[role][i] != 0) {
                svc_ui->elem_set_visible(mod_ctx, s_live.promote[role][i], false);
            }
        }
    }

    svc_ui->pane_add_section(mod_ctx, left, "Story");
    s_live.lastStoryStatus = story::statusRml();
    s_live.storyStatus = addRml(left, s_live.lastStoryStatus);
    UiControlDesc catchUp = UI_CONTROL_DESC_INIT;
    catchUp.kind = UI_CONTROL_BUTTON;
    s_live.lastCatchUp = catchUpLabel();
    catchUp.label = s_live.lastCatchUp.c_str();
    catchUp.help_rml = "<p>Go where your story says you should be: follow a teammate whose story "
                       "event moved them, or load the place your flags say you are in.</p>";
    catchUp.on_pressed = onCatchUp;
    catchUp.is_disabled = catchUpDisabled;
    addControl(left, catchUp, &s_live.catchUp);
    return MOD_OK;
}

ModResult updatePlayersTab(ModContext*, void*, ModError*) {
    setRml(s_live.players, s_live.lastPlayers, playersRml());
    const std::vector<uint32_t> targets = teleportTargets();
    for (size_t i = 0; i < kTeleportSlots; ++i) {
        const uint32_t id = i < targets.size() ? targets[i] : 0;
        if (s_live.teleport[i] == 0) {
            continue;
        }
        if (id != s_live.teleportIds[i]) {
            s_live.teleportIds[i] = id;
            svc_ui->elem_set_visible(mod_ctx, s_live.teleport[i], id != 0);
        }
        if (id != 0) {
            setLabel(s_live.teleport[i], s_live.lastTeleportLabels[i], teleportLabel(id));
        }
    }
    std::string status = teleport::statusMessage();
    if (targets.empty()) {
        status = status.empty() ? "No other players." : status;
    }
    setText(s_live.teleportStatus, s_live.lastTeleportStatus, std::move(status));
    for (const PromoteRole role : {kRoomOwner, kTeamLeader}) {
        const std::vector<uint32_t> ids = promoteTargets(role);
        for (size_t i = 0; i < kTeleportSlots; ++i) {
            const uint32_t id = i < ids.size() ? ids[i] : 0;
            if (s_live.promote[role][i] == 0) {
                continue;
            }
            if (id != s_live.promoteIds[role][i]) {
                s_live.promoteIds[role][i] = id;
                svc_ui->elem_set_visible(mod_ctx, s_live.promote[role][i], id != 0);
            }
            if (id != 0) {
                const char* what = role == kRoomOwner ? " Room Owner" : " Team Leader";
                setLabel(s_live.promote[role][i], s_live.lastPromoteLabels[role][i],
                    "Make " + clientName(id) + what);
            }
        }
    }
    setRml(s_live.storyStatus, s_live.lastStoryStatus, story::statusRml());
    setLabel(s_live.catchUp, s_live.lastCatchUp, catchUpLabel());
    return MOD_OK;
}

// --- window, menu tab, Mods panel

void onWindowClosed(ModContext*, UiWindowHandle, void*) {
    s_window = 0;
    s_live = {};
}

void onMenuSelected(ModContext*, void*) {
    openWindow();
}

void onOpenFromPanel(ModContext*, void*) {
    openWindow();
}

ModResult buildModsPanel(ModContext*, UiElementHandle pane, void*, ModError*) {
    s_panelLastStatus = Session::instance().statusText();
    svc_ui->pane_add_text(mod_ctx, pane, s_panelLastStatus.c_str(), &s_panelStatus);
    s_panelLastTeam = team_game::panelLine();
    svc_ui->pane_add_text(mod_ctx, pane, s_panelLastTeam.c_str(), &s_panelTeam);
    UiControlDesc open = UI_CONTROL_DESC_INIT;
    open.kind = UI_CONTROL_BUTTON;
    open.label = "Open Twili-Together";
    open.on_pressed = onOpenFromPanel;
    addControl(pane, open);
    return MOD_OK;
}

ModResult updateModsPanel(ModContext*, void*, ModError*) {
    setText(s_panelStatus, s_panelLastStatus, Session::instance().statusText());
    setText(s_panelTeam, s_panelLastTeam, team_game::panelLine());
    return MOD_OK;
}

}  // namespace

bool openWindow() {
    if (s_window != 0) {
        return true;
    }
    UiTabDesc tabs[3] = {UI_TAB_DESC_INIT, UI_TAB_DESC_INIT, UI_TAB_DESC_INIT};
    tabs[0].title = "Connection";
    tabs[0].build = buildConnectionTab;
    tabs[0].update = updateConnectionTab;
    tabs[1].title = "Room";
    tabs[1].build = buildRoomTab;
    tabs[1].update = updateRoomTab;
    tabs[2].title = "Players";
    tabs[2].build = buildPlayersTab;
    tabs[2].update = updatePlayersTab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = std::size(tabs);
    desc.on_closed = onWindowClosed;
    if (svc_ui->window_push(mod_ctx, &desc, &s_window) != MOD_OK) {
        TwiliLog.warn("[ui] could not open the Twili-Together window");
        s_window = 0;
        return false;
    }
    return true;
}

void closeWindow() {
    if (s_window != 0) {
        svc_ui->window_close(mod_ctx, s_window);
    }
    s_window = 0;
    s_live = {};
}

bool windowOpen() {
    return s_window != 0;
}

UiElementHandle colorControl() {
    return s_live.color;
}

ModResult registerWindow() {
    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = buildModsPanel;
    panel.update = updateModsPanel;
    if (svc_ui->register_mods_panel(mod_ctx, &panel) != MOD_OK) {
        TwiliLog.warn("[ui] could not add the Mods window panel");
    }
    UiMenuTabDesc desc = UI_MENU_TAB_DESC_INIT;
    desc.label = "Twili-Together";
    desc.on_selected = onMenuSelected;
    return svc_ui->register_menu_tab(mod_ctx, &desc, &s_menuTab);
}

void shutdownWindow() {
    s_window = 0;
    s_live = {};
    s_menuTab = 0;
    s_panelStatus = 0;
    s_panelTeam = 0;
    forgetStoryPrompts();
}

}  // namespace twili::ui
