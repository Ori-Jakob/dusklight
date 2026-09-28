#include "ui/TwiliWindow.hpp"

#include "core/Config.hpp"
#include "core/Layout.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "story/StoryUi.hpp"
#include "teleport/Teleport.hpp"
#include "ui/ColorMath.hpp"
#include "ui/StoryPrompt.hpp"
#include "ui/Toasts.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstring>
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
    UiElementHandle players = 0;
    std::array<UiElementHandle, kTeleportSlots> teleport{};
    std::array<uint32_t, kTeleportSlots> teleportIds{};
    UiElementHandle teleportStatus = 0;
    UiElementHandle storyStatus = 0;
    UiElementHandle catchUp = 0;
    std::string lastStatus;
    std::string lastTag;
    std::string lastRoomInfo;
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
        "<p>Players on the same team share flags, items and save progress. Takes effect on the "
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

// While connected as a non-owner the controls show the room's values and refuse edits;
// otherwise they edit the defaults pushed to the room whenever we own it.
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
};

const RoomInt kDifficultyOptions[] = {
    {Var::EnemyCountMultiplier, &RoomState::enemyCountMultiplier, "Enemy Count",
        "<p>Multiplier for enemy spawn count. 100% is normal.</p>"},
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
        control.is_disabled = roomLockedPredicate;
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

ModResult buildRoomTab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle,
    void*, ModError*) {
    s_live = {};
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
    return MOD_OK;
}

// --- Players tab

std::string playersRml() {
    const Session& session = Session::instance();
    if (!session.isConnected()) {
        return "Not connected.";
    }
    std::string rml;
    for (const auto& [id, c] : session.clients()) {
        std::string tags;
        if (c.self) tags += " (you)";
        if (id == session.roomState().ownerClientId) tags += " [owner]";
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
        rml += fmt::format(
            "<p><span class=\"twili-swatch\" style=\"background-color: {};\"></span> {}{}</p>",
            colorCss(c.colorR, c.colorG, c.colorB), escapeRml(clientName(id)), escapeRml(tags));
        std::string details = where;
        // The form comes with PLAYER_UPDATE, which only players in our stage and layer send.
        if (c.isSaveLoaded && !c.self && c.hasPlayerUpdate) {
            details += c.transformStatus != 0 ? " (wolf)" : " (human)";
        }
        details += c.teamId.empty() ? " · no team" : " · team " + c.teamId;
        if (!c.modVersion.empty()) {
            details += " · " + c.modVersion;
        }
        rml += "<p class=\"twili-player-detail\">" + escapeRml(details) + "</p>";
        if (const std::string story = story::clientLine(id); !story.empty()) {
            rml += "<p class=\"twili-player-detail\">Story: " + escapeRml(story) + "</p>";
        }
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
    UiControlDesc open = UI_CONTROL_DESC_INIT;
    open.kind = UI_CONTROL_BUTTON;
    open.label = "Open Twili-Together";
    open.on_pressed = onOpenFromPanel;
    addControl(pane, open);
    return MOD_OK;
}

ModResult updateModsPanel(ModContext*, void*, ModError*) {
    setText(s_panelStatus, s_panelLastStatus, Session::instance().statusText());
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
    forgetStoryPrompts();
}

}  // namespace twili::ui
