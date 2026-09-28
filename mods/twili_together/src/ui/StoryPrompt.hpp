#pragma once

#include <functional>
#include <string>

namespace twili::ui {

struct StoryPromptProps {
    std::string title;
    std::string bodyRml;
    std::string acceptLabel;
    std::string declineLabel;
    std::function<void()> onAccept;
    // Also runs on cancel, so closing the prompt any way counts as "not now".
    std::function<void()> onDecline;
};

// The story prompt ("Kira was captured - follow?") over gameplay; replaces a showing one.
void showStoryPrompt(StoryPromptProps props);
void closeStoryPrompt();
bool storyPromptShowing();

// The "Catch up to story" confirmation over the Twili-Together window.
void pushStoryConfirm(StoryPromptProps props);

// Any host or mod document visible (they take the gamepad).
bool anyDocumentVisible();

// Mod shutdown: the host closes the dialogs itself.
void forgetStoryPrompts();

}  // namespace twili::ui
