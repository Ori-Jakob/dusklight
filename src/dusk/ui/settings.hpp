#pragma once
#include "window.hpp"

#include <string_view>
#include <vector>

namespace dusk::ui {

class SettingsWindow : public Window {
public:
    SettingsWindow(bool prelaunch = false);

    void update() override;
    void hide(bool close) override;
    bool show_tab(std::string_view title);
    void focus_row(std::string_view label);

protected:
    void add_tab(const Rml::String& title, TabBuilder builder);
    bool try_focus_row();

    bool mPrelaunch;
    std::vector<Rml::String> mTabTitles;
    Rml::String mPendingRow;
    int mPendingRowFrames = 0;
};

bool open_settings(std::string_view tab, std::string_view row);

}  // namespace dusk::ui