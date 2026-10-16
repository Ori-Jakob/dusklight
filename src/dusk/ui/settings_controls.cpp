#include "settings_controls.hpp"

#include "bool_button.hpp"
#include "number_button.hpp"
#include "window.hpp"

#include "dusk/speedrun.h"

#include <algorithm>

namespace dusk::ui {

namespace detail {

std::function<bool()> locked_by(const std::function<Rml::String()>& unavailableReason) {
    if (!unavailableReason) {
        return {};
    }
    return [unavailableReason] { return !unavailableReason().empty(); };
}

std::function<bool()> unless_locked(
    std::function<bool()> locked, std::function<bool()> isDisabled) {
    if (!isDisabled || !locked) {
        return isDisabled;
    }
    // A locked row stays focusable so its reason can be read, even while its dependency is unmet
    return [locked = std::move(locked), isDisabled = std::move(isDisabled)] {
        return !locked() && isDisabled();
    };
}

}  // namespace detail

int float_setting_percent(ConfigVar<float>& var) {
    return static_cast<int>(var.getValue() * 100.0f + 0.5f);
}

SelectButton& config_bool_select(
    Pane& leftPane, Pane& rightPane, ConfigVar<bool>& var, ConfigBoolProps props) {
    std::function<bool()> locked = detail::locked_by(props.unavailableReason);
    std::function<bool()> isModified = std::move(props.isModified);
    if (!isModified) {
        isModified = [&var] { return var.getValue() != var.getDefaultValue(); };
    }
    auto& button = leftPane.add_child<BoolButton>(BoolButton::Props{
        .key = std::move(props.key),
        .icon = std::move(props.icon),
        .getValue = [&var] { return var.getValue(); },
        .setValue =
            [&var, callback = std::move(props.onChange)](bool value) {
                if (value == var.getValue()) {
                    return;
                }
                var.setValue(value);
                config::save();
                if (callback) {
                    callback(value);
                }
            },
        .isDisabled = detail::unless_locked(locked, std::move(props.isDisabled)),
        .isModified = std::move(isModified),
        .valueOverride = std::move(props.valueOverride),
        .isLocked = locked,
    });
    leftPane.register_control(button, rightPane,
        [helpText = std::move(props.helpText),
            unavailableReason = std::move(props.unavailableReason),
            helpSuffix = std::move(props.helpSuffix)](Pane& pane) {
            pane.clear();
            Rml::String reason = unavailableReason ? unavailableReason() : Rml::String{};
            if (reason.empty() && helpSuffix) {
                reason = helpSuffix();
            }
            pane.add_rml(compose_help(helpText, reason));
        });
    return button;
}

void add_speedrun_disabled_option(Pane& leftPane, Pane& rightPane, ConfigVar<bool>& var,
    const Rml::String& key, const Rml::String& helpText) {
    config_bool_select(leftPane, rightPane, var,
        {
            .key = key,
            .helpText = helpText,
            .isDisabled = [] { return speedrun::isActive(); },
        });
}

SelectButton& config_percent_select(Pane& leftPane, Pane& rightPane, ConfigVar<float>& var,
    Rml::String key, Rml::String helpText, int min, int max, int step,
    std::function<bool()> isDisabled) {
    auto& button = leftPane.add_child<NumberButton>(NumberButton::Props{
        .key = std::move(key),
        .getValue = [&var] { return float_setting_percent(var); },
        .setValue =
            [&var, min, max](int value) {
                var.setValue(std::clamp(value, min, max) / 100.0f);
                config::save();
            },
        .isDisabled = std::move(isDisabled),
        .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
        .min = min,
        .max = max,
        .step = step,
        .suffix = "%",
    });
    leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane& pane) {
        pane.clear();
        pane.add_rml(helpText);
    });
    return button;
}

SelectButton& config_int_select(Pane& leftPane, Pane& rightPane, ConfigVar<int>& var,
    Rml::String key, Rml::String helpText, int min, int max, int step,
    std::function<bool()> isDisabled, std::function<void(int)> onChange, std::string suffix) {
    auto& button = leftPane.add_child<NumberButton>(NumberButton::Props{
        .key = std::move(key),
        .getValue = [&var] { return var.getValue(); },
        .setValue =
            [&var, min, max, callback = std::move(onChange)](int value) {
                const int clampedValue = std::clamp(value, min, max);
                var.setValue(clampedValue);
                config::save();
                if (callback) {
                    callback(clampedValue);
                }
            },
        .isDisabled = std::move(isDisabled),
        .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
        .min = min,
        .max = max,
        .step = step,
        .suffix = suffix,
    });
    leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane& pane) {
        pane.clear();
        pane.add_text(helpText);
    });
    return button;
}

void graphics_tuner_control(
    Window& window, Pane& leftPane, Pane& rightPane, const GraphicsTunerProps& props) {
    const auto setting = GraphicsSetting::of(props.option);
    std::function<bool()> locked = detail::locked_by(props.unavailableReason);
    leftPane.register_control(
        leftPane
            .add_select_button({
                .key = props.title,
                .getValue =
                    [setting, valueOverride = props.valueOverride] {
                        if (valueOverride) {
                            if (Rml::String value = valueOverride(); !value.empty()) {
                                return value;
                            }
                        }
                        return setting.text();
                    },
                .isDisabled = detail::unless_locked(locked, props.isDisabled),
                .isModified = [setting] { return setting.isModified(); },
                .submit = false,
                .isLocked = locked,
            })
            .on_nav_command([&window, props, locked](Rml::Event&, NavCommand cmd) {
                if (cmd == NavCommand::Confirm || cmd == NavCommand::Left ||
                    cmd == NavCommand::Right) {
                    // The button's own handler plays the warning for a locked row
                    if (!locked || !locked()) {
                        window.push(std::make_unique<GraphicsTuner>(props));
                    }
                    return true;
                }
                return false;
            }),
        rightPane,
        [helpText = props.helpText, unavailableReason = props.unavailableReason](Pane& pane) {
            pane.clear();
            pane.add_rml(
                compose_help(helpText, unavailableReason ? unavailableReason() : Rml::String{}));
        });
}

SelectButton& config_action_select(Pane& leftPane, Pane& rightPane, ActionProps props) {
    std::function<bool()> locked = detail::locked_by(props.unavailableReason);
    auto& button = leftPane.add_select_button({
        .key = std::move(props.key),
        .getValue = std::move(props.getValue),
        .isDisabled = detail::unless_locked(locked, std::move(props.isDisabled)),
        .submit = false,
        .isLocked = locked,
    });
    button.on_nav_command(
        [locked, onPressed = std::move(props.onPressed)](Rml::Event&, NavCommand cmd) {
            if (locked && locked()) {
                // The button's own handler already played the warning
                return cmd == NavCommand::Confirm || cmd == NavCommand::Left ||
                       cmd == NavCommand::Right;
            }
            if (cmd == NavCommand::Confirm) {
                mDoAud_seStartMenu(onPressed && onPressed() ? kSoundClick : kSoundWarning);
                return true;
            }
            return false;
        });
    leftPane.register_control(button, rightPane,
        [helpText = std::move(props.helpText),
            unavailableReason = std::move(props.unavailableReason)](Pane& pane) {
            pane.clear();
            pane.add_rml(
                compose_help(helpText, unavailableReason ? unavailableReason() : Rml::String{}));
        });
    return button;
}

Rml::String compose_help(const Rml::String& helpText, std::string_view reason) {
    if (reason.empty()) {
        return helpText;
    }
    return helpText + "<br/><br/>" + Rml::String{reason};
}

}  // namespace dusk::ui
