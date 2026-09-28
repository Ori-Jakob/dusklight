#include "ui/StoryPrompt.hpp"

#include "core/Log.hpp"

#include <mods/svc/ui.h>

#include <iterator>
#include <utility>

namespace twili::ui {
namespace {

struct Prompt {
    UiDialogHandle handle = 0;
    StoryPromptProps props;
};

// [0] the gameplay prompt, [1] the catch-up confirmation, [2] team game questions.
Prompt s_prompts[3];

Prompt& slot(void* userData) {
    return s_prompts[reinterpret_cast<uintptr_t>(userData) % std::size(s_prompts)];
}

// Forget the closing dialog before the callback, which may open the next prompt.
void onAccept(ModContext*, UiDialogHandle, void* userData) {
    Prompt& p = slot(userData);
    auto accept = std::move(p.props.onAccept);
    p = {};
    if (accept) {
        accept();
    }
}

void onDecline(ModContext*, UiDialogHandle, void* userData) {
    Prompt& p = slot(userData);
    auto decline = std::move(p.props.onDecline);
    p = {};
    if (decline) {
        decline();
    }
}

void push(uintptr_t index, StoryPromptProps props) {
    Prompt& p = s_prompts[index];
    if (p.handle != 0) {
        svc_ui->dialog_close(mod_ctx, p.handle);
        p = {};
    }
    p.props = std::move(props);
    void* userData = reinterpret_cast<void*>(index);

    UiDialogAction actions[2] = {UI_DIALOG_ACTION_INIT, UI_DIALOG_ACTION_INIT};
    actions[0].label = p.props.acceptLabel.c_str();
    actions[0].on_pressed = onAccept;
    actions[0].user_data = userData;
    actions[1].label = p.props.declineLabel.c_str();
    actions[1].on_pressed = onDecline;
    actions[1].user_data = userData;

    UiDialogDesc desc = UI_DIALOG_DESC_INIT;
    desc.title = p.props.title.c_str();
    desc.body_rml = p.props.bodyRml.c_str();
    desc.icon = "question-mark";
    desc.actions = actions;
    desc.action_count = std::size(actions);
    desc.on_dismiss = onDecline;
    desc.user_data = userData;
    if (svc_ui->dialog_push(mod_ctx, &desc, &p.handle) != MOD_OK) {
        TwiliLog.warn("[ui] could not show \"{}\"", p.props.title);
        p = {};
    }
}

}  // namespace

void showStoryPrompt(StoryPromptProps props) {
    push(0, std::move(props));
}

void closeStoryPrompt() {
    Prompt& p = s_prompts[0];
    if (p.handle != 0) {
        svc_ui->dialog_close(mod_ctx, p.handle);
    }
    p = {};
}

bool storyPromptShowing() {
    return s_prompts[0].handle != 0;
}

void pushStoryConfirm(StoryPromptProps props) {
    push(1, std::move(props));
}

void showTeamPrompt(StoryPromptProps props) {
    push(2, std::move(props));
}

bool teamPromptShowing() {
    return s_prompts[2].handle != 0;
}

void forgetStoryPrompts() {
    for (Prompt& p : s_prompts) {
        p = {};
    }
}

bool anyDocumentVisible() {
    bool visible = false;
    return svc_ui->is_any_document_visible(mod_ctx, &visible) == MOD_OK && visible;
}

}  // namespace twili::ui
