#include "story/StoryUi.hpp"

#include "core/SaveGate.hpp"
#include "story/StoryLog.hpp"
#include "story/StoryState.hpp"
#include "teleport/Teleport.hpp"
#include "ui/Toasts.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_stage.h"

#include <fmt/format.h>

namespace twili::story {

using detail::ago;
using detail::moveTitle;
using ui::escapeRml;

std::string statusRml() {
    if (!isSaveLoaded()) {
        return "Not in game.";
    }
    const detail::State& st = detail::state();
    const CatchUpPlan& p = st.plan;
    std::string html;
    if (const StorySegment* seg = segmentById(p.segment)) {
        html += escapeRml(fmt::format("Your story: {}.", seg->text)) + "<br/>";
        html += p.inconsistent ?
                    escapeRml(fmt::format("You are not where it says ({}).", seg->place)) :
                p.behind ? escapeRml(fmt::format("Your story goes on in {}.", seg->place)) :
                           std::string("You are where your story says.");
        html += "<br/>";
    }
    const detail::TeamMove& team = st.team;
    if (team.valid) {
        const char* s = team.satisfied ? "caught up" : team.declined ? "not followed" : "open";
        html += escapeRml(fmt::format(
                    "Latest team move: {} ({}, {}).", moveTitle(team.move), ago(team.move.at), s)) +
                "<br/>";
    }
    const detail::LoadState& l = st.load;
    if (l.phase == LoadPhase::Waiting) {
        html += escapeRml(fmt::format("Waiting to load {} ({}).", l.entrance.stage,
                    teleport::reasonText(l.reason.empty() ? "loading" : l.reason))) +
                "<br/>";
    } else if (l.phase == LoadPhase::Loading) {
        html += escapeRml(fmt::format("Loading {}...", l.entrance.stage)) + "<br/>";
    } else if (l.phase == LoadPhase::Failed) {
        html += escapeRml(l.message) + "<br/>";
    }
    // A follow plan's reason repeats the team move line above.
    if (p.moveId.empty() || !team.valid) {
        html += escapeRml(p.reason);
    }
    return html;
}

std::string clientLine(uint32_t clientId) {
    const auto& moves = detail::state().clientMoves;
    const auto it = moves.find(clientId);
    if (it == moves.end()) {
        return {};
    }
    return fmt::format("{}, {}", it->second.text, ago(it->second.at));
}

std::string catchUpTitle() {
    const std::string& title = detail::state().plan.title;
    return title.empty() ? std::string("Catch up to story") : title;
}

const char* catchUpBlockCode() {
    return detail::catchUpBlockCode();
}

std::string catchUpConfirmRml() {
    const CatchUpPlan& plan = detail::state().plan;
    std::string html;
    if (plan.kind == CatchUpPlan::Kind::TeleportToPlayer) {
        html = escapeRml(fmt::format("Teleport to {}? {}", plan.place, plan.reason));
    } else {
        html = "Load <b>" + escapeRml(plan.place) + "</b>" +
               escapeRml(fmt::format(
                   " ({}, room {})? {}", plan.entrance.stage, plan.entrance.room, plan.reason));
    }
    if (plan.form != Form::Any) {
        html += escapeRml(fmt::format(" You will arrive as a {}.", formName(plan.form)));
    }
    html += " Your items and story progress are kept.";
    stage_stag_info_class* info = dComIfGp_getStageStagInfo();
    if (info != nullptr && dStage_stagInfo_GetSTType(info) == ST_DUNGEON) {
        html += "<br/>You are in a dungeon: doors and switches that only stay open while you are "
                "inside reset when you leave; small keys, chests and maps are kept.";
    }
    return html;
}

void startCatchUp() {
    detail::startCatchUpPlan();
}

void forgetLearnedStory() {
    storylog::forget();
}

}  // namespace twili::story
