#include "story/StoryUi.hpp"

// No story sync until P5 replaces this file.
namespace twili::story {

std::string statusRml() {
    return "Story sync is not available in this version.";
}

std::string clientLine(uint32_t) {
    return {};
}

std::string catchUpTitle() {
    return "Catch up to story";
}

const char* catchUpBlockCode() {
    return "unavailable";
}

std::string catchUpConfirmRml() {
    return {};
}

void startCatchUp() {}

}  // namespace twili::story
