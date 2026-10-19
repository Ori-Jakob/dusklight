#include "commands.hpp"

#include "dusk/config.hpp"
#include "dusk/dusk.h"
#include "dusk/imgui/ImGuiConsole.hpp"
#include "dusk/input_replay.hpp"
#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/render/camera_cuts.hpp"
#include "dusk/render/draw_tags.hpp"
#include "dusk/render/lights.hpp"
#include "dusk/render/material_tags.hpp"
#include "dusk/render/model_registry.hpp"
#include "dusk/render/render_bridge.hpp"
#include "dusk/render/render_settings.hpp"
#include "dusk/settings.h"
#include "dusk/ui/settings_graphics.hpp"
#include "game_clock.h"

#include "c/c_damagereaction.h"
#include "d/actor/d_a_alink.h"
#include "d/d_camera.h"
#include "d/d_com_inf_actor.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_layer_iter.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_node.h"

#include "JSystem/JUtility/JUTGamePad.h"
#include "SSystem/SComponent/c_math.h"
#include "SSystem/SComponent/c_sxyz.h"
#include "SSystem/SComponent/c_xyz.h"

#include <aurora/render.hpp>
#include <aurora/trace.hpp>
#include <borealis/io.hpp>
#include <borealis/version.h>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace dusk {
namespace {

static constexpr int kMaxHistory = 64;

static std::vector<std::string> SplitArgs(std::string_view input) {
    std::vector<std::string> args;
    std::string cur;
    bool quoted = false;
    bool hadQuotes = false;
    for (char c : input) {
        if (c == '"') {
            quoted = !quoted;
            hadQuotes = true;
        } else if (!quoted && (c == ' ' || c == '\t')) {
            if (!cur.empty() || hadQuotes) {
                args.push_back(std::move(cur));
                cur.clear();
                hadQuotes = false;
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty() || hadQuotes) {
        args.push_back(std::move(cur));
    }
    return args;
}

static std::optional<long> ParseLong(const std::string& s) {
    if (s.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 0);
    return (end != s.c_str() && *end == '\0') ? std::optional<long>{v} : std::nullopt;
}

static std::optional<float> ParseFloat(const std::string& s) {
    if (s.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    return (end != s.c_str() && *end == '\0') ? std::optional<float>{v} : std::nullopt;
}

static const char* ActorShortName(s16 profname) {
    const char* n = dStage_getName(profname, -1);
    return n ? n : "?";
}

// Resolves @found / @link / @<id> to a process pointer, outputting an error on failure
static base_process_class* ParseProcArg(
    const std::string& s, unsigned int foundProcId, const CommandOutput& output) {
    if (s.empty() || s[0] != '@') {
        output("Error: proc reference must start with @");
        return nullptr;
    }
    const std::string inner = s.substr(1);
    unsigned int id;
    if (inner == "found") {
        if (foundProcId == 0) {
            output("Error: @found is not set");
            return nullptr;
        }
        id = foundProcId;
    } else if (inner == "link") {
        auto* player = dComIfGp_getPlayer(0);
        if (player == nullptr) {
            output("Error: player not available");
            return nullptr;
        }
        id = (unsigned int)fpcM_GetID(player);
    } else {
        const auto v = ParseLong(inner);
        if (!v) {
            output("Error: invalid proc ID");
            return nullptr;
        }
        id = (unsigned int)*v;
    }
    auto* proc = fpcM_SearchByID(id);
    if (proc == nullptr) {
        output(fmt::format(FMT_STRING("Error: proc {} not found"), id));
        return nullptr;
    }
    return proc;
}

// Like ParseProcArg but ensures the proc is an actor
static fopAc_ac_c* ParseActorArg(
    const std::string& s, unsigned int foundProcId, const CommandOutput& output) {
    auto* proc = ParseProcArg(s, foundProcId, output);
    if (proc == nullptr) {
        return nullptr;
    }
    if (!fopAcM_IsActor(proc)) {
        output("Error: proc is not an actor");
        return nullptr;
    }
    return static_cast<fopAc_ac_c*>(proc);
}

static std::optional<s16> ParseActorId(const std::string& s) {
    if (const auto v = ParseLong(s)) {
        return (s16)*v;
    }
    const auto* entry = dStage_searchNameCI(s.c_str());
    return entry ? std::optional<s16>{entry->procname} : std::nullopt;
}

static std::optional<cXyz> ParseXYZ(const std::vector<std::string>& args, size_t i) {
    const auto x = ParseFloat(args[i]), y = ParseFloat(args[i + 1]), z = ParseFloat(args[i + 2]);
    return (x && y && z) ? std::optional<cXyz>{cXyz(*x, *y, *z)} : std::nullopt;
}

static bool TryAngle(
    s16& out, const std::vector<std::string>& args, size_t i, const CommandOutput& output) {
    if (args.size() <= i) {
        return true;
    }
    const auto a = ParseLong(args[i]);
    if (!a) {
        output("Error: invalid angle");
        return false;
    }
    out = (s16)*a;
    return true;
}

// A player warp drops every history, while any other actor only needs its own model's
static void NoteActorMoved(const fopAc_ac_c* actor) {
    if (actor == dComIfGp_getPlayer(0)) {
        render::note_camera_cut(render::CutSource::Warp);
    } else {
        render::note_teleport(actor->model);
    }
}

static std::string actorLine(const base_process_class* proc) {
    const auto* ac = static_cast<const fopAc_ac_c*>(proc);
    return fmt::format(FMT_STRING("procId={} 0x{:04X} ({}) @ ({:.2f}, {:.2f}, {:.2f}) room={}"),
        (unsigned int)proc->id, (unsigned int)(u16)proc->profname, ActorShortName(proc->profname),
        ac->current.pos.x, ac->current.pos.y, ac->current.pos.z, (int)ac->current.roomNo);
}

static void recurseLayer(void* p, int (*callback)(void*, void*), void* ctx) {
    auto* proc = static_cast<base_process_class*>(p);
    if (fpcBs_Is_JustOfType(g_fpcNd_type, proc->subtype)) {
        fpcLyIt_OnlyHere(&static_cast<process_node_class*>(p)->layer, callback, ctx);
    }
}

struct ListContext {
    s16 targetId;
    std::vector<std::string>* output;
};
struct FindContext {
    s16 targetId;
    std::vector<base_process_class*> matches;
};

static int ListActorCallback(void* p, void* ctx) {
    auto* proc = static_cast<base_process_class*>(p);
    auto* context = static_cast<ListContext*>(ctx);
    if (fopAcM_IsActor(proc) && (context->targetId < 0 || proc->profname == context->targetId)) {
        context->output->push_back("  " + actorLine(proc));
    }
    recurseLayer(p, ListActorCallback, ctx);
    return 1;
}

static int FindActorCallback(void* p, void* ctx) {
    auto* proc = static_cast<base_process_class*>(p);
    auto* context = static_cast<FindContext*>(ctx);
    if (fopAcM_IsActor(proc) && proc->profname == context->targetId) {
        context->matches.push_back(proc);
    }
    recurseLayer(p, FindActorCallback, ctx);
    return 1;
}

struct CameraFlyState {
    bool active    = false;
    cXyz startEye;
    cXyz startCenter;
    cXyz endEye;
    cXyz endCenter;
    float duration = 0.0f;
    float elapsed  = 0.0f;
};
CameraFlyState s_cameraFly;

static dCamera_c* getCamera() { return dCam_getBody(); }

static cXyz anglesToDir(s16 h, s16 v) {
    return cXyz(cM_scos(v) * cM_ssin(h), cM_ssin(v), cM_scos(v) * cM_scos(h));
}

static void cameraToAngles(dCamera_c* cam, s16& h, s16& v) {
    const cXyz eye = cam->iEye();
    const cXyz ctr = cam->iCenter();
    const float dx = ctr.x - eye.x;
    const float dy = ctr.y - eye.y;
    const float dz = ctr.z - eye.z;
    const float hDist = sqrtf(dx * dx + dz * dz);
    h = cM_atan2s(dx, dz);
    v = cM_atan2s(dy, hDist);
}

static float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static constexpr float kCamTargetDist = 100.0f;

static constexpr long kMaxTraceFrames = 10000;
static constexpr const char* kTraceExtension = ".gxtrace";
static constexpr const char* kTraceUsage =
    "Usage: render.trace [-f] <frames 1-10000> [file]  |  render.trace stop | status";

static std::tm LocalTimeNow() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return local;
}

static std::filesystem::path ResolveTracePath(std::string_view arg) {
    std::filesystem::path path;
    if (arg.empty()) {
        const char* stage = dComIfGp_getStartStageName();
        path = borealis::io::fs_path_from_utf8(fmt::format(FMT_STRING("{}_r{}_{:%Y%m%d-%H%M%S}"),
            (stage != nullptr && stage[0] != '\0') ? stage : "trace",
            (int)dComIfGp_getStartStageRoomNo(), LocalTimeNow()));
    } else {
        path = borealis::io::fs_path_from_utf8(arg);
    }
    if (path.is_relative()) {
        path = ConfigPath / "traces" / path;
    }
    if (!path.has_extension()) {
        path += kTraceExtension;
    }
    return path;
}

static void WriteTraceMetadata(const std::filesystem::path& tracePath, long frames) {
    nlohmann::json meta;
    meta["format"] = 1;
    meta["trace"] = borealis::io::fs_path_to_string(tracePath.filename());
    meta["framesRequested"] = frames;
    meta["createdUtc"] =
        fmt::format(FMT_STRING("{:%Y-%m-%dT%H:%M:%SZ}"), fmt::gmtime(std::time(nullptr)));
    meta["build"] = BOREALIS_APP_DESCRIBE;
    meta["backend"] = std::string(backend_name(auroraInfo.backend));

    const char* stage = dComIfGp_getStartStageName();
    meta["stage"] = {
        {"name", stage != nullptr ? stage : ""},
        {"room", (int)dComIfGp_getStartStageRoomNo()},
        {"point", (int)dComIfGp_getStartStagePoint()},
        {"layer", (int)dComIfGp_getStartStageLayer()},
    };
    if (auto* player = dComIfGp_getPlayer(0)) {
        meta["player"] = {
            {"pos", {player->current.pos.x, player->current.pos.y, player->current.pos.z}},
            {"angleY", (int)player->shape_angle.y},
        };
    } else {
        meta["player"] = nullptr;
    }
    if (dCamera_c* camera = dCam_getBody()) {
        const cXyz eye = camera->iEye();
        const cXyz center = camera->iCenter();
        meta["camera"] = {
            {"eye", {eye.x, eye.y, eye.z}},
            {"center", {center.x, center.y, center.z}},
        };
    } else {
        meta["camera"] = nullptr;
    }
    meta["timeOfDay"] = dComIfGs_getTime();
    if (const auto replay = input_replay::position(); replay.mode != input_replay::Mode::Idle) {
        meta["inputReplay"] = {{"file", replay.file}, {"tick", replay.tick}};
    }

    const auto& settings = getSettings();
    meta["settings"] = {
        {settings.game.enableFrameInterpolation.getName(),
            (int)settings.game.enableFrameInterpolation.getValue()},
        {settings.game.internalResolutionScale.getName(),
            settings.game.internalResolutionScale.getValue()},
        {settings.video.maxFrameRate.getName(), settings.video.maxFrameRate.getValue()},
    };
    nlohmann::json requested = nlohmann::json::object();
    nlohmann::json effective = nlohmann::json::object();
    for (const auto& field : render::describe_fields()) {
        requested[std::string(field.key)] = field.requested;
        effective[std::string(field.key)] = field.effective;
    }
    meta["graphics"] = {
        {"preset", std::string(render::preset_name(render::detect_preset()))},
        {"requested", std::move(requested)},
        {"effective", std::move(effective)},
    };

    auto metaPath = tracePath;
    metaPath.replace_extension(".dusk.json");
    std::ofstream out(metaPath, std::ios::binary | std::ios::trunc);
    out << meta.dump(2) << '\n';
    if (!out) {
        DuskLog.warn("render.trace: failed to write {}", borealis::io::fs_path_to_string(metaPath));
    }
}

static std::string ForeignTraceOwner(const CommandState& state) {
    if (state.replaySource) {
        return "started from the console";
    }
    return input_replay::mode() == input_replay::Mode::Idle ? "started elsewhere" :
                                                              "started by input replay";
}

// Replayed commands already reach the log through the replay's own output.
static void TraceReply(
    const CommandState& state, const CommandOutput& output, const std::string& line) {
    if (!state.replaySource) {
        DuskLog.info("{}", line);
    }
    output(line);
}

static void TraceError(
    const CommandState& state, const CommandOutput& output, const std::string& line) {
    if (!state.replaySource) {
        DuskLog.warn("{}", line);
    }
    output(line);
}

static void RunRenderTrace(
    const std::vector<std::string>& args, CommandState& state, const CommandOutput& output) {
    size_t i = 1;
    bool overwrite = false;
    if (args.size() > i && args[i] == "-f") {
        overwrite = true;
        ++i;
    }
    const std::string sub = args.size() > i ? args[i] : "status";

    if (sub == "status") {
        if (!aurora::trace::is_capturing()) {
            TraceReply(state, output, "No trace capture in progress");
        } else if (state.tracePath.empty()) {
            TraceReply(state, output,
                fmt::format(
                    FMT_STRING("Trace capture in progress ({})"), ForeignTraceOwner(state)));
        } else {
            TraceReply(state, output,
                fmt::format(FMT_STRING("Trace capture in progress: {} ({} frames requested)"),
                    state.tracePath, state.traceFrames));
        }
        return;
    }

    if (sub == "stop") {
        if (!aurora::trace::is_capturing()) {
            TraceReply(state, output, "No trace capture in progress");
            return;
        }
        aurora::trace::stop_capture();
        TraceReply(state, output,
            fmt::format(FMT_STRING("Trace stopped early: {}"),
                state.tracePath.empty() ?
                    fmt::format(FMT_STRING("({})"), ForeignTraceOwner(state)) :
                    state.tracePath));
        state.tracePath.clear();
        return;
    }

    const auto frames = ParseLong(sub);
    if (!frames || *frames < 1 || *frames > kMaxTraceFrames || args.size() > i + 2) {
        TraceError(state, output, kTraceUsage);
        return;
    }
    if (aurora::trace::is_capturing()) {
        TraceError(state, output,
            fmt::format(
                FMT_STRING(
                    "Error: a trace capture is already running ({}); use 'render.trace stop'"),
                state.tracePath.empty() ? ForeignTraceOwner(state) : state.tracePath));
        return;
    }

    const auto path = ResolveTracePath(args.size() > i + 1 ? args[i + 1] : std::string{});
    const std::string pathString = borealis::io::fs_path_to_string(path);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        TraceError(state, output,
            fmt::format(FMT_STRING("Error: cannot create '{}': {}"),
                borealis::io::fs_path_to_string(path.parent_path()), ec.message()));
        return;
    }
    if (!overwrite && std::filesystem::exists(path, ec)) {
        TraceError(state, output,
            fmt::format(
                FMT_STRING("Error: '{}' already exists (use 'render.trace -f ...' to overwrite)"),
                pathString));
        return;
    }

    const aurora::trace::CaptureOptions options{
        .path = path,
        .frameCount = static_cast<uint32_t>(*frames),
        .label = borealis::io::fs_path_to_string(path.stem()),
    };
    if (!aurora::trace::start_capture(options)) {
        TraceError(state, output,
            fmt::format(FMT_STRING("Error: aurora refused to start the capture for '{}': {}"),
                pathString, aurora::trace::capture_status().error));
        return;
    }
    WriteTraceMetadata(path, *frames);
    state.tracePath = pathString;
    state.traceFrames = static_cast<unsigned int>(*frames);
    TraceReply(
        state, output, fmt::format(FMT_STRING("Capturing {} frames to {}"), *frames, pathString));
}

static constexpr const char* kRenderSettingsUsage = "Usage: render.settings [all]";
static constexpr const char* kRenderFeaturesUsage = "Usage: render.features";
static constexpr const char* kRenderSetUsage = "Usage: render.set <graphics.key> <value>";
static constexpr const char* kRenderSelfTestUsage = "Usage: render.selftest";
static constexpr const char* kRenderGraphDumpUsage = "Usage: render.graph.dump [file]";
static constexpr const char* kRenderPresetUsage =
    "Usage: render.preset [original|low|medium|high|ultra|recommended|reset]";
static constexpr const char* kRenderGraphDumpFile = "render_graph.json";
static constexpr const char* kRenderHintsUsage = "Usage: render.hints [reset|ids]";
static constexpr const char* kRenderObjectsUsage = "Usage: render.objects [log <frames>]";
static constexpr const char* kRenderTagsUsage = "Usage: render.tags [pending on|off]";

static const char* YesNo(bool value) {
    return value ? "yes" : "no";
}

static void RunRenderSettings(
    const std::vector<std::string>& args, CommandState& state, const CommandOutput& output) {
    if (args.size() > 2 || (args.size() == 2 && args[1] != "all")) {
        TraceError(state, output, kRenderSettingsUsage);
        return;
    }
    const bool all = args.size() == 2;
    const uint32_t latency = render::last_latch_latency();
    const bool inSync =
        render::same_settings(aurora::render::effective_settings(), render::last_clamped());
    TraceReply(state, output,
        fmt::format(
            FMT_STRING("preset {} (base {}), pushes {}, pending {}, latency {} frames, in sync {}"),
            render::preset_name(render::detect_preset()),
            render::preset_name(render::base_preset()), render::push_count(),
            YesNo(render::is_pending()), latency == 0 ? std::string{"-"} : std::to_string(latency),
            YesNo(inSync)));

    size_t differing = 0;
    for (const auto& field : render::describe_fields()) {
        const bool differs = field.differs();
        if (differs) {
            ++differing;
        }
        if (all || differs) {
            TraceReply(state, output,
                fmt::format(FMT_STRING("{} requested={} clamped={} effective={}"), field.key,
                    field.requested, field.clamped, field.effective));
        }
    }
    TraceReply(state, output, fmt::format(FMT_STRING("{} fields differ"), differing));
}

static void RunRenderFeatures(
    const std::vector<std::string>& args, CommandState& state, const CommandOutput& output) {
    if (args.size() != 1) {
        TraceError(state, output, kRenderFeaturesUsage);
        return;
    }
    for (const auto feature : render::all_features()) {
        const auto support = aurora::render::feature_support(feature);
        TraceReply(state, output,
            fmt::format(FMT_STRING("{} supported={} reason={} active={} ready={}"),
                render::feature_name(feature), support.supported ? 1 : 0,
                render::reason_name(support.reason),
                aurora::render::feature_active(feature) ? 1 : 0,
                aurora::render::feature_ready(feature) ? 1 : 0));
    }
}

static void RunRenderSet(
    const std::vector<std::string>& args, CommandState& state, const CommandOutput& output) {
    if (args.size() != 3) {
        TraceError(state, output, kRenderSetUsage);
        return;
    }
    const std::string& key = args[1];
    config::ConfigVarBase* var = key.starts_with("graphics.") ? config::GetConfigVar(key) : nullptr;
    if (var == nullptr) {
        TraceError(
            state, output, fmt::format(FMT_STRING("Error: unknown graphics setting '{}'"), key));
        return;
    }
    try {
        var->getImpl()->loadFromArg(*var, args[2]);
    } catch (const std::exception& e) {
        TraceError(state, output, fmt::format(FMT_STRING("Error: {}"), e.what()));
        return;
    }
    // The override layer loads silently, so the change callback that marks the bridge never runs
    render::mark_dirty();

    std::string requested;
    for (const auto& field : render::describe_fields()) {
        if (field.key == key) {
            requested = field.requested;
            break;
        }
    }
    TraceReply(
        state, output, fmt::format(FMT_STRING("{} = {} (override, not saved)"), key, requested));
}

static void RunRenderSelfTest(
    const std::vector<std::string>& args, CommandState& state, const CommandOutput& output) {
    if (args.size() != 1) {
        TraceError(state, output, kRenderSelfTestUsage);
        return;
    }
    render::SelfTestReport report;
    render::bridge_self_test(report);
    render::settings_self_test(report);
    ui::graphics_tab_self_test(report);
    render::hints_self_test(report);
    render::draw_tags_self_test(report);
    render::registry_self_test(report);
    render::material_tags_self_test(report);
    render::lights_self_test(report);
    render::environment_self_test(report);
    render::cuts_self_test(report);
    for (const auto& failure : report.failed) {
        TraceError(state, output, "FAIL " + failure);
    }
    for (const auto& skip : report.skipped) {
        TraceReply(state, output, "SKIP " + skip);
    }
    const std::string skipped = report.skipped.empty() ?
                                    std::string{} :
                                    fmt::format(FMT_STRING(", {} skipped"), report.skipped.size());
    TraceReply(state, output,
        fmt::format(FMT_STRING("render.selftest: {} passed{}, {} failed"), report.passed, skipped,
            report.failed.size()));
}

static bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(
        a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

static std::optional<GraphicsPreset> ParseGraphicsPreset(std::string_view name) {
    for (const auto preset : {GraphicsPreset::Original, GraphicsPreset::Low, GraphicsPreset::Medium,
             GraphicsPreset::High, GraphicsPreset::Ultra})
    {
        if (EqualsIgnoreCase(name, render::preset_name(preset))) {
            return preset;
        }
    }
    return std::nullopt;
}

static void RunRenderPreset(
    const std::vector<std::string>& args, CommandState& state, const CommandOutput& output) {
    if (args.size() > 2) {
        TraceError(state, output, kRenderPresetUsage);
        return;
    }
    if (args.size() == 1) {
        TraceReply(state, output,
            fmt::format(FMT_STRING("Graphics preset: {}, recommended: {}"),
                render::preset_display_name(), render::preset_name(render::recommended_preset())));
        return;
    }
    if (EqualsIgnoreCase(args[1], "reset")) {
        render::reset_to_preset();
    } else if (EqualsIgnoreCase(args[1], "recommended")) {
        render::apply_preset(render::recommended_preset());
    } else if (const auto preset = ParseGraphicsPreset(args[1])) {
        render::apply_preset(*preset);
    } else {
        TraceError(state, output, kRenderPresetUsage);
        return;
    }
    config::save();
    TraceReply(state, output,
        fmt::format(FMT_STRING("Graphics preset: {}"), render::preset_display_name()));
}

static void RunRenderGraphDump(
    const std::vector<std::string>& args, CommandState& state, const CommandOutput& output) {
    if (args.size() > 2) {
        TraceError(state, output, kRenderGraphDumpUsage);
        return;
    }
    std::filesystem::path root = ConfigPath.lexically_normal();
    if (!root.has_filename()) {
        root = root.parent_path();
    }
    std::filesystem::path path =
        (ConfigPath / borealis::io::fs_path_from_utf8(
                          args.size() == 2 ? args[1] : std::string{kRenderGraphDumpFile}))
            .lexically_normal()
            .make_preferred();
    const std::string pathString = borealis::io::fs_path_to_string(path);
    // Replayed recordings run this unattended, so it may only write inside the config folder
    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty() || relative == "." || *relative.begin() == ".." || !path.has_filename()) {
        TraceError(state, output,
            fmt::format(FMT_STRING("Error: '{}' is not a file inside '{}'"), pathString,
                borealis::io::fs_path_to_string(root)));
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        TraceError(state, output,
            fmt::format(FMT_STRING("Error: cannot create '{}': {}"),
                borealis::io::fs_path_to_string(path.parent_path()), ec.message()));
        return;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << aurora::render::graph_dump_json(true);
    out.close();
    if (!out) {
        TraceError(state, output, fmt::format(FMT_STRING("Error: cannot write '{}'"), pathString));
        return;
    }
    TraceReply(state, output, fmt::format(FMT_STRING("Wrote {}"), pathString));
}

static void RunRenderHints(
    const std::vector<std::string>& args, CommandState& state, const CommandOutput& output) {
    if (args.size() > 2) {
        TraceError(state, output, kRenderHintsUsage);
        return;
    }
    if (args.size() == 1) {
        for (const auto& line : render::describe_hints()) {
            TraceReply(state, output, line);
        }
        return;
    }
    if (args[1] == "ids") {
        for (const auto& line : render::describe_hint_ids()) {
            TraceReply(state, output, line);
        }
    } else if (args[1] == "reset") {
        render::reset_hint_stats();
        TraceReply(state, output, "hints: counters reset at the next painter frame");
    } else {
        TraceError(state, output, kRenderHintsUsage);
    }
}

static void RunRenderObjects(
    const std::vector<std::string>& args, CommandState& state, const CommandOutput& output) {
    if (args.size() == 1) {
        for (const auto& line : render::registry_report()) {
            TraceReply(state, output, line);
        }
        for (const auto& line : render::tag_report()) {
            TraceReply(state, output, line);
        }
        return;
    }
    uint32_t frames = 0;
    const std::string_view count = args.size() == 3 ? std::string_view{args[2]} : "";
    const auto [end, error] = std::from_chars(count.data(), count.data() + count.size(), frames);
    if (args.size() != 3 || args[1] != "log" || error != std::errc{} ||
        end != count.data() + count.size())
    {
        TraceError(state, output, kRenderObjectsUsage);
        return;
    }
    render::set_object_log(frames);
    TraceReply(state, output,
        fmt::format(
            FMT_STRING("render.objects: logging until frame {}"), render::object_log_until()));
}

static void RunRenderTags(
    const std::vector<std::string>& args, CommandState& state, const CommandOutput& output) {
    if (args.size() == 3 && args[1] == "pending" && (args[2] == "on" || args[2] == "off")) {
        render::set_pending_tags_enabled(args[2] == "on");
    } else if (args.size() != 1) {
        TraceError(state, output, kRenderTagsUsage);
        return;
    }
    TraceReply(state, output,
        fmt::format(
            FMT_STRING("pending review tags: {}"), render::pending_tags_enabled() ? "on" : "off"));
    if (args.size() == 1) {
        for (const auto& line : render::material_tag_report()) {
            TraceReply(state, output, line);
        }
    }
}

using RenderTableCommand = bool (*)(
    std::span<const std::string> args, std::vector<std::string>& out);

static void RunRenderTable(RenderTableCommand command, const std::vector<std::string>& args,
    CommandState& state, const CommandOutput& output) {
    std::vector<std::string> lines;
    const bool ok = command(std::span(args).subspan(1), lines);
    for (const auto& line : lines) {
        if (ok) {
            TraceReply(state, output, line);
        } else {
            TraceError(state, output, line);
        }
    }
}

}  // namespace

bool isCameraDetached() { return s_cameraFly.active; }

void pollCommands(CommandState& state, const CommandOutput& output) {
    if (state.tracePath.empty() || aurora::trace::is_capturing()) {
        return;
    }
    const auto status = aurora::trace::capture_status();
    std::string line;
    if (status.state == aurora::trace::CaptureState::Finished) {
        line = fmt::format(
            FMT_STRING("Trace complete: {} ({} frames)"), state.tracePath, status.framesCaptured);
        if (!state.replaySource) {
            DuskLog.info("{}", line);
        }
    } else {
        line = fmt::format(FMT_STRING("Trace failed: {}: {}"), state.tracePath, status.error);
        if (!state.replaySource) {
            DuskLog.warn("{}", line);
        }
    }
    output(line);
    state.tracePath.clear();
}

bool isRenderTraceActive() {
    return aurora::trace::is_capturing();
}

void processCameraCommands() {
    if (!s_cameraFly.active) { return; }
    dCamera_c* cam = getCamera();
    if (cam == nullptr) { s_cameraFly.active = false; return; }

    cXyz eye, center;

    if (s_cameraFly.duration <= 0.0f) {
        eye    = s_cameraFly.endEye;
        center = s_cameraFly.endCenter;
    } else {
        s_cameraFly.elapsed += dusk::game_clock::kSimPeriod;
        const float t = smoothstep(s_cameraFly.elapsed / s_cameraFly.duration);
        eye    = s_cameraFly.startEye    + (s_cameraFly.endEye    - s_cameraFly.startEye)    * t;
        center = s_cameraFly.startCenter + (s_cameraFly.endCenter - s_cameraFly.startCenter) * t;

        if (s_cameraFly.elapsed >= s_cameraFly.duration) {
            eye    = s_cameraFly.endEye;
            center = s_cameraFly.endCenter;
            s_cameraFly.startEye    = s_cameraFly.endEye;
            s_cameraFly.startCenter = s_cameraFly.endCenter;
            s_cameraFly.duration    = 0.0f;
        }
    }

    cam->Reset(center, eye);
    cam->mDebugFlyCam.initialized = false;
}

void runCommand(std::string_view cmdLine, CommandState& state, const CommandOutput& output) {
    if (state.echoEnabled) {
        output(fmt::format(FMT_STRING("> {}"), cmdLine));
    }

    if (!cmdLine.empty()) {
        if (state.history.empty() || state.history.back() != cmdLine) {
            state.history.push_back(std::string(cmdLine));
            if ((int)state.history.size() > kMaxHistory) {
                state.history.erase(state.history.begin());
            }
        }
    }

    auto args = SplitArgs(cmdLine);
    if (args.empty()) {
        return;
    }
    const auto& cmd = args[0];
    if (!input_replay::accept_console_command(cmd, cmdLine, state.replaySource, output)) {
        return;
    }

    auto requirePlayer = [&]() -> daAlink_c* {
        auto* p = (daAlink_c*)dComIfGp_getPlayer(0);
        if (p == nullptr) {
            output("Error: player not available");
        }
        return p;
    };

    if (cmd == "tp") {
        auto* player = requirePlayer();
        if (player == nullptr) {
            return;
        }

        if (args.size() >= 2 && args[1].starts_with('@')) {
            auto* ac = ParseActorArg(args[1], state.foundProcId, output);
            if (ac == nullptr) {
                return;
            }

            if (args.size() >= 3 && args[2].starts_with('@')) {
                auto* destAc = ParseActorArg(args[2], state.foundProcId, output);
                if (destAc == nullptr) {
                    return;
                }
                const cXyz destPos = destAc->current.pos;
                ac->current.pos = destPos;
                NoteActorMoved(ac);
                output(fmt::format(FMT_STRING("Moved actor {} to ({:.2f}, {:.2f}, {:.2f})"), ac->id,
                    destPos.x, destPos.y, destPos.z));
                return;
            }

            if (args.size() >= 5) {
                const auto pos = ParseXYZ(args, 2);
                if (!pos) {
                    output("Error: invalid coordinates");
                    return;
                }
                ac->current.pos = *pos;
                NoteActorMoved(ac);
                if (!TryAngle(ac->shape_angle.y, args, 5, output)) {
                    return;
                }
                output(fmt::format(FMT_STRING("Moved actor {} to ({:.2f}, {:.2f}, {:.2f})"), ac->id,
                    pos->x, pos->y, pos->z));
                return;
            }

            player->current.pos = ac->current.pos;
            render::note_camera_cut(render::CutSource::Warp);
            output(fmt::format(FMT_STRING("Teleported to actor {} ({:.2f}, {:.2f}, {:.2f})"),
                ac->id, ac->current.pos.x, ac->current.pos.y, ac->current.pos.z));
            return;
        }

        if (args.size() < 4) {
            output("Usage: tp <x> <y> <z> [angle]  |  tp @<procId> [<x> <y> <z> [angle] | @link]");
            return;
        }
        const auto pos = ParseXYZ(args, 1);
        if (!pos) {
            output("Error: invalid coordinates");
            return;
        }
        player->current.pos = *pos;
        render::note_camera_cut(render::CutSource::Warp);
        if (!TryAngle(player->shape_angle.y, args, 4, output)) {
            return;
        }
        output(fmt::format(
            FMT_STRING("Teleported to ({:.2f}, {:.2f}, {:.2f})"), pos->x, pos->y, pos->z));
        return;
    }

    if (cmd == "spawn") {
        if (args.size() < 2) {
            output("Usage: spawn <actorId> [params] [x y z] [angle]");
            return;
        }
        auto* player = requirePlayer();
        if (player == nullptr) {
            return;
        }

        const auto actorId = ParseActorId(args[1]);
        if (!actorId) {
            output("Error: unknown actor ID or name");
            return;
        }

        long paramsL = -1;
        if (args.size() >= 3) {
            const auto p = ParseLong(args[2]);
            if (!p) {
                output("Error: invalid params");
                return;
            }
            paramsL = *p;
        }

        cXyz pos = player->current.pos;
        if (args.size() >= 6) {
            const auto spawnPos = ParseXYZ(args, 3);
            if (!spawnPos) {
                output("Error: invalid spawn coordinates");
                return;
            }
            pos = *spawnPos;
        }

        s16 angleY = 0;
        if (!TryAngle(angleY, args, 6, output)) {
            return;
        }
        csXyz angle;
        angle.set(0, angleY, 0);
        cXyz scale(1.0f, 1.0f, 1.0f);

        layer_class* savedLayer = fpcLy_CurrentLayer();
        base_process_class* playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
        if (playScene != nullptr) {
            fpcLy_SetCurrentLayer(&((process_node_class*)playScene)->layer);
        }
        unsigned int result = fopAcM_create(
            *actorId, (u32)paramsL, &pos, player->current.roomNo, &angle, &scale, (s8)-1);
        fpcLy_SetCurrentLayer(savedLayer);

        output(result != 0 ? fmt::format(FMT_STRING("Spawned actorId=0x{:04X} procId={}"),
                                 (unsigned int)(u16)*actorId, result) :
                             fmt::format(FMT_STRING("Failed to spawn actorId=0x{:04X}"),
                                 (unsigned int)(u16)*actorId));
        return;
    }

    if (cmd == "reset") {
        JUTGamePad::C3ButtonReset::sResetSwitchPushing = true;
        output("Soft reset triggered");
        return;
    }

    if (cmd == "warp") {
        if (args.size() < 4) {
            output("Usage: warp <stageName> <point> <roomNo> [layer=-1]");
            output("  e.g.  warp F_SP121 0 0");
            return;
        }
        const auto pointL = ParseLong(args[2]), roomL = ParseLong(args[3]);
        if (!pointL || !roomL) {
            output("Error: invalid point or room number");
            return;
        }
        long layerL = -1;
        if (args.size() >= 5) {
            const auto l = ParseLong(args[4]);
            if (!l) {
                output("Error: invalid layer");
                return;
            }
            layerL = *l;
        }
        state.lastWarpStage = args[1];
        dComIfGp_setNextStage(state.lastWarpStage.c_str(), (s16)*pointL, (s8)*roomL, (s8)layerL);
        output(fmt::format(FMT_STRING("Warping to {} point={} room={} layer={}"), args[1],
            (int)(s16)*pointL, (int)(s8)*roomL, (int)(s8)layerL));
        return;
    }

    if (cmd == "list") {
        s16 targetId = -1;
        if (args.size() >= 2) {
            const auto id = ParseActorId(args[1]);
            if (!id) {
                output("Error: unknown actor ID or name");
                return;
            }
            targetId = *id;
        }
        std::vector<std::string> results;
        ListContext ctx{targetId, &results};
        fpcLyIt_OnlyHere(fpcLy_RootLayer(), ListActorCallback, &ctx);
        output(results.empty() ? "No matching actors found" :
                                 fmt::format(FMT_STRING("Found {} actor(s):"), results.size()));
        for (const auto& r : results) {
            output(r);
        }
        return;
    }

    if (cmd == "killall") {
        if (args.size() < 2) {
            output("Usage: killall <actorId|name>");
            return;
        }
        const auto targetId = ParseActorId(args[1]);
        if (!targetId) {
            output("Error: unknown actor ID or name");
            return;
        }
        FindContext ctx{*targetId, {}};
        fpcLyIt_OnlyHere(fpcLy_RootLayer(), FindActorCallback, &ctx);
        for (auto* proc : ctx.matches) {
            fpcM_Delete(proc);
        }
        output(fmt::format(FMT_STRING("Deleted {} actor(s) of type {} ({})"),
            (int)ctx.matches.size(), (unsigned int)(u16)*targetId, ActorShortName(*targetId)));
        return;
    }

    if (cmd == "heal") {
        auto* player = requirePlayer();
        if (player == nullptr) {
            return;
        }
        const u16 maxLife = dComIfGs_getMaxLife() / 5 * 4;
        u16 newLife = maxLife;
        if (args.size() >= 2) {
            const auto amount = ParseLong(args[1]);
            if (!amount) {
                output("Error: invalid amount");
                return;
            }
            newLife =
                (u16)std::max(0L, std::min((long)maxLife, (long)dComIfGs_getLife() + *amount));
        }
        dComIfGs_setLife(newLife);
        output(fmt::format(FMT_STRING("Health: {}/{}"), (int)newLife, (int)maxLife));
        return;
    }

    if (cmd == "kill") {
        if (args.size() >= 2) {
            auto* proc = ParseProcArg(args[1], state.foundProcId, output);
            if (proc == nullptr) {
                return;
            }
            fpcM_Delete(proc);
            output(fmt::format(FMT_STRING("Deleted proc {}"), proc->id));
        } else {
            auto* player = requirePlayer();
            if (player == nullptr) {
                return;
            }
            dComIfGs_setLife(0);
            output("Set Link's health to 0");
        }
        return;
    }

    if (cmd == "freeze" || cmd == "unfreeze") {
        const bool doFreeze = (cmd == "freeze");
        if (args.size() < 2) {
            g_dComIfAc_gameInfo.mPause = doFreeze;
            output(doFreeze ? "Global freeze on" : "Global freeze off");
            return;
        }
        auto* ac = ParseActorArg(args[1], state.foundProcId, output);
        if (ac == nullptr) {
            return;
        }
        if (doFreeze) {
            fpcM_PauseEnable(ac, 1);
            output(fmt::format(FMT_STRING("Froze actor {}"), (unsigned int)ac->id));
        } else {
            fpcM_PauseDisable(ac, 1);
            output(fmt::format(FMT_STRING("Unfroze actor {}"), (unsigned int)ac->id));
        }
        return;
    }

    if (cmd == "rate") {
        if (args.size() >= 2) {
            const auto hz = ParseLong(args[1]);
            if (!hz || *hz <= 0) {
                output("Error: rate must be a positive integer");
                return;
            }
            dusk::game_clock::set_sim_rate((float)*hz);
        }
        output(fmt::format(FMT_STRING("Sim rate: {:.0f} hz"), dusk::game_clock::get_sim_rate()));
        return;
    }

    if (cmd == "ebf") {
        if (args.size() >= 2) {
            const auto val = ParseLong(args[1]);
            if (!val || *val < 0 || *val > 255) {
                output("Error: value must be 0-255");
                return;
            }
            cDmr_SkipInfo = (u8)*val;
        }
        output(fmt::format(FMT_STRING("EBF = {}"), (int)cDmr_SkipInfo));
        return;
    }

    if (cmd == "time") {
        if (args.size() >= 2) {
            const auto t = ParseFloat(args[1]);
            if (!t || *t < 0.0f || *t > 360.0f) {
                output("Error: time must be a float between 0 and 360");
                return;
            }
            dKy_instant_timechg(*t);
        }
        output(fmt::format(FMT_STRING("Time: {:.2f} ({}:{:02d})"), dComIfGs_getTime(),
            dKy_getdaytime_hour(), dKy_getdaytime_minute()));
        return;
    }

    if (cmd == "rupees") {
        const u16 maxRupees = dComIfGs_getRupeeMax();
        if (args.size() >= 2) {
            const auto amount = ParseLong(args[1]);
            if (!amount) {
                output("Error: invalid amount");
                return;
            }
            dComIfGs_setRupee((u16)std::max(0L, std::min((long)maxRupees, *amount)));
        }
        output(fmt::format(FMT_STRING("Rupees: {}/{}"), (int)dComIfGs_getRupee(), (int)maxRupees));
        return;
    }

    if (cmd == "find") {
        if (args.size() < 2) {
            if (state.foundProcId == 0) {
                output("@found is not set");
                return;
            }
            auto* proc = fpcM_SearchByID(state.foundProcId);
            output(proc != nullptr && fopAcM_IsActor(proc) ?
                       "@found = " + actorLine(proc) :
                       fmt::format(FMT_STRING("@found = {} (no longer exists)"),
                           (unsigned int)state.foundProcId));
            return;
        }

        const auto targetId = ParseActorId(args[1]);
        if (!targetId) {
            output("Error: unknown actor ID or name");
            return;
        }

        int targetN = 1;
        if (args.size() >= 3) {
            const auto n = ParseLong(args[2]);
            if (!n || *n < 1) {
                output("Error: index must be >= 1");
                return;
            }
            targetN = (int)*n;
        }

        FindContext ctx{*targetId, {}};
        fpcLyIt_OnlyHere(fpcLy_RootLayer(), FindActorCallback, &ctx);

        if (ctx.matches.empty()) {
            output(fmt::format(FMT_STRING("No actors found for '{}'"), args[1]));
            return;
        }

        std::sort(ctx.matches.begin(), ctx.matches.end(),
            [](const base_process_class* a, const base_process_class* b) { return a->id < b->id; });

        if (targetN > (int)ctx.matches.size()) {
            output(fmt::format(
                FMT_STRING("Error: only {} actor(s) of that type exist"), (int)ctx.matches.size()));
            return;
        }

        auto* picked = ctx.matches[(size_t)(targetN - 1)];
        state.foundProcId = picked->id;
        output(fmt::format(
            FMT_STRING("@found [{}/{}] {}"), targetN, (int)ctx.matches.size(), actorLine(picked)));
        return;
    }

    if (cmd == "camera") {
        const std::string sub = args.size() >= 2 ? args[1] : "";

        if (sub == "attach") {
            s_cameraFly.active = false;
            output("Camera attached");
            return;
        }

        auto* cam = getCamera();
        if (cam == nullptr) { output("Error: camera not available"); return; }

        if (sub == "detach") {
            s_cameraFly.active      = true;
            s_cameraFly.duration    = 0.0f;
            s_cameraFly.startEye    = s_cameraFly.endEye    = cam->iEye();
            s_cameraFly.startCenter = s_cameraFly.endCenter = cam->iCenter();
            output("Camera detached");
            return;
        }

        if (sub == "tp") {
            // camera tp @<ref>
            if (args.size() >= 3 && !args[2].empty() && args[2][0] == '@') {
                auto* ac = ParseActorArg(args[2], state.foundProcId, output);
                if (ac == nullptr) { return; }
                const cXyz actorPos = ac->current.pos;
                const cXyz dir = anglesToDir(ac->shape_angle.y, 0);
                const cXyz newEye    = cXyz(actorPos.x - dir.x * kCamTargetDist,
                                            actorPos.y - dir.y * kCamTargetDist + 50.0f,
                                            actorPos.z - dir.z * kCamTargetDist);
                s_cameraFly.active = true; s_cameraFly.duration = 0.0f;
                s_cameraFly.startEye = s_cameraFly.endEye = newEye;
                s_cameraFly.startCenter = s_cameraFly.endCenter = actorPos;
                    output(fmt::format(FMT_STRING("Camera moved to actor {}"), ac->id));
                return;
            }
            // camera tp x y z [h] [v]
            if (args.size() < 5) {
                output("Usage: camera tp <x> <y> <z> [h_angle] [v_angle]  |  camera tp @<ref>");
                return;
            }
            const auto pos = ParseXYZ(args, 2);
            if (!pos) { output("Error: invalid coordinates"); return; }
            s16 h = 0, v = 0;
            cameraToAngles(cam, h, v);
            if (args.size() >= 6) {
                const auto hv = ParseLong(args[5]);
                if (!hv) { output("Error: invalid h_angle"); return; }
                h = (s16)*hv;
            }
            if (args.size() >= 7) {
                const auto vv = ParseLong(args[6]);
                if (!vv) { output("Error: invalid v_angle"); return; }
                v = (s16)*vv;
            }
            const cXyz dir = anglesToDir(h, v);
            const cXyz center = cXyz(pos->x + dir.x * kCamTargetDist,
                                     pos->y + dir.y * kCamTargetDist,
                                     pos->z + dir.z * kCamTargetDist);
            s_cameraFly.active = true; s_cameraFly.duration = 0.0f;
            s_cameraFly.startEye = s_cameraFly.endEye = *pos;
            s_cameraFly.startCenter = s_cameraFly.endCenter = center;
            output(fmt::format(FMT_STRING("Camera teleported to ({:.2f}, {:.2f}, {:.2f})"),
                pos->x, pos->y, pos->z));
            return;
        }

        if (sub == "pos") {
            s16 h = 0, v = 0;
            cameraToAngles(cam, h, v);
            const cXyz eye = cam->iEye();
            output(fmt::format(FMT_STRING("Camera: ({:.2f}, {:.2f}, {:.2f}) h={} v={}"),
                eye.x, eye.y, eye.z, (int)h, (int)v));
            return;
        }

        if (sub == "fly") {
            // camera fly <time> <x> <y> <z> [h] [v]  |  camera fly <time> @<ref>
            if (args.size() < 4) {
                output("Usage: camera fly <time> <x> <y> <z> [h_angle] [v_angle]  |  camera fly <time> @<ref>");
                return;
            }
            const auto t = ParseFloat(args[2]);
            if (!t || *t <= 0) { output("Error: invalid time"); return; }
            const float flyTime = *t;

            cXyz targetPos;
            s16 h = 0, v = 0;

            if (!args[3].empty() && args[3][0] == '@') {
                auto* ac = ParseActorArg(args[3], state.foundProcId, output);
                if (ac == nullptr) { return; }
                targetPos = ac->current.pos;
                h = ac->shape_angle.y;
                v = 0;
            } else {
                if (args.size() < 6) {
                    output("Usage: camera fly <time> <x> <y> <z> [h_angle] [v_angle]  |  camera fly <time> @<ref>");
                    return;
                }
                const auto pos = ParseXYZ(args, 3);
                if (!pos) { output("Error: invalid coordinates"); return; }
                targetPos = *pos;
                cameraToAngles(cam, h, v);
                if (args.size() >= 7) {
                    const auto hv = ParseLong(args[6]); if (!hv) { output("Error: invalid h_angle"); return; } h = (s16)*hv;
                }
                if (args.size() >= 8) {
                    const auto vv = ParseLong(args[7]); if (!vv) { output("Error: invalid v_angle"); return; } v = (s16)*vv;
                }
            }

            const cXyz dir = anglesToDir(h, v);
            const cXyz endCenter = cXyz(targetPos.x + dir.x * kCamTargetDist,
                                        targetPos.y + dir.y * kCamTargetDist,
                                        targetPos.z + dir.z * kCamTargetDist);

            s_cameraFly.active      = true;
            s_cameraFly.startEye    = cam->iEye();
            s_cameraFly.startCenter = cam->iCenter();
            s_cameraFly.endEye      = targetPos;
            s_cameraFly.endCenter   = endCenter;
            s_cameraFly.duration    = flyTime;
            s_cameraFly.elapsed     = 0.0f;
            output(fmt::format(FMT_STRING("Camera flying to ({:.2f}, {:.2f}, {:.2f}) over {:.1f}s"),
                targetPos.x, targetPos.y, targetPos.z, flyTime));
            return;
        }

        output("Usage: camera detach | attach | tp ... | fly ... | pos");
        return;
    }

    if (cmd == "echo") {
        if (args.size() >= 2 && args[1] == "off") {
            state.echoEnabled = false;
            output("Echo disabled");
        } else if (args.size() >= 2 && args[1] == "on") {
            state.echoEnabled = true;
            output("Echo enabled");
        } else {
            output(state.echoEnabled ? "Echo: on" : "Echo: off");
        }
        return;
    }

    if (cmd == "transform") {
        auto* player = requirePlayer();
        if (player == nullptr) { return; }
        player->procCoMetamorphoseInit();
        output("Transforming");
        return;
    }

    if (cmd == "angle") {
        auto* player = requirePlayer();
        if (player == nullptr) { return; }
        if (args.size() >= 2) {
            const auto a = ParseLong(args[1]);
            if (!a) { output("Error: invalid angle"); return; }
            player->shape_angle.y = (s16)*a;
            output(fmt::format(FMT_STRING("Angle set to {}"), (int)(s16)*a));
        } else {
            output(fmt::format(FMT_STRING("Angle: {}"), (int)player->shape_angle.y));
        }
        return;
    }

    if (cmd == "pos") {
        auto* player = requirePlayer();
        if (player == nullptr) {
            return;
        }
        output(fmt::format(FMT_STRING("pos: {:.4f} {:.4f} {:.4f}"), player->current.pos.x,
            player->current.pos.y, player->current.pos.z));
        output(
            fmt::format(FMT_STRING("stage: {}  room: {}  entry: {}"), dComIfGp_getStartStageName(),
                (int)player->current.roomNo, (int)dComIfGp_getStartStagePoint()));
        return;
    }

    if (cmd == "input.status") {
        input_replay::print_status(output);
        return;
    }

    if (cmd == "input.stop") {
        input_replay::stop(output);
        return;
    }

    if (cmd == "input.hash") {
        input_replay::print_hash(output);
        return;
    }

    if (cmd == "render.trace") {
        RunRenderTrace(args, state, output);
        return;
    }

    if (cmd == "render.settings") {
        RunRenderSettings(args, state, output);
        return;
    }

    if (cmd == "render.features") {
        RunRenderFeatures(args, state, output);
        return;
    }

    if (cmd == "render.set") {
        RunRenderSet(args, state, output);
        return;
    }

    if (cmd == "render.selftest") {
        RunRenderSelfTest(args, state, output);
        return;
    }

    if (cmd == "render.graph.dump") {
        RunRenderGraphDump(args, state, output);
        return;
    }

    if (cmd == "render.preset") {
        RunRenderPreset(args, state, output);
        return;
    }

    if (cmd == "render.hints") {
        RunRenderHints(args, state, output);
        return;
    }

    if (cmd == "render.objects") {
        RunRenderObjects(args, state, output);
        return;
    }

    if (cmd == "render.tags") {
        RunRenderTags(args, state, output);
        return;
    }

    if (cmd == "render.lights") {
        RunRenderTable(&render::lights_command, args, state, output);
        return;
    }

    if (cmd == "render.env") {
        RunRenderTable(&render::environment_command, args, state, output);
        return;
    }

    if (cmd == "help") {
        output("@<ref>  =  @<procId> | @found | @link");
        output("");
        output("angle [value]                               Get or set Link's facing angle");
        output("camera detach | attach                      Detach or reattach camera");
        output("camera pos                                  Print camera position and angles");
        output("camera tp <x> <y> <z> [h] [v]               Teleport camera (h/v are s16 angles)");
        output("camera tp @<ref>                            Teleport camera to actor");
        output("camera fly <t> <x> <y> <z> [h] [v]          Fly camera over t seconds (h/v are s16 angles)");
        output("camera fly <t> @<ref>                       Fly camera to actor");
        output("ebf [0-255]                                 Get or set cDmr_SkipInfo");
        output("echo on | off                               Enable or disable command echo");
        output("find <id|name> [n=1]                        Store nth actor as @found");
        output("freeze [@<ref>]                             Freeze globally, or freeze actor");
        output("heal [amount]                               Heal to max, or by relative amount");
        output("input.status | input.stop | input.hash      Input replay status, stop, state hash");
        output("kill                                        Set Link health to 0");
        output("kill @<ref>                                 Delete proc");
        output("killall <id|name>                           Delete all actors of a type");
        output("list [id|name]                              List actors in scene");
        output("pos                                         Print player position and stage");
        output("rate [hz]                                   Get or set sim rate (1-480, default 30)");
        output("render.env [show|check|counts|log <n>]      Renderer environment state");
        output(
            "render.graph.dump [file]                    Write the compiled render graph as JSON");
        output("render.hints [reset|ids]                    Hint counts, camera cuts, scene epoch");
        output("render.lights [list|slots|check|counts]     Renderer scene lights");
        output("render.lights log <n>                       Log the next n light lists");
        output("render.objects [log <frames>]               Renderer objects and scopes for the "
               "last frame");
        output("render.preset <name|recommended|reset>      Apply or show the graphics preset");
        output("render.selftest                             Check the renderer bridge and Graphics "
               "tab tables");
        output("render.set <graphics.key> <value>           Override a graphics setting until "
               "restart");
        output("render.settings [all] | render.features     Renderer settings and feature support");
        output("render.tags [pending on|off]                Tagged materials; toggle pending "
               "review tags");
        output("render.trace [-f] <frames> [file]           Capture N frames to a GX trace file");
        output("render.trace stop | status                  Stop or query the trace capture");
        output("reset                                       Soft reset");
        output("rupees [amount]                             Get or set rupee count");
        output("spawn <id|name> [params] [x y z] [angle]    Spawn actor");
        output("time [0-360]                                Get or set time of day");
        output("tp <x> <y> <z> [angle]                      Teleport Link to coords");
        output("tp @<ref>                                   Teleport Link to actor");
        output("tp @<ref> <x> <y> <z> [angle]               Move actor to coords");
        output("tp @<ref> @<ref>                            Move actor to actor");
        output("transform                                   Force transform");
        output("unfreeze [@<ref>]                           Unfreeze globally, or unfreeze actor");
        output("warp <stage> <point> <room> [layer]         Warp to stage");
        return;
    }

    output(fmt::format(FMT_STRING("Unknown command '{}' (try 'help')"), cmd));
}

}  // namespace dusk
