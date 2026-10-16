#pragma once

#include "graphics_tuner.hpp"
#include "pane.hpp"
#include "select_button.hpp"
#include "ui.hpp"

#include "dusk/config.hpp"
#include "dusk/settings.h"
#include "m_Do/m_Do_audio.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dusk::ui {

class Window;

struct ConfigBoolProps {
    Rml::String key;
    Rml::String icon;
    Rml::String helpText;
    std::function<void(bool)> onChange;
    std::function<bool()> isDisabled;
    std::function<Rml::String()> unavailableReason;
    std::function<Rml::String()> valueOverride;
    std::function<bool()> isModified;
    std::function<Rml::String()> helpSuffix;
};

template <typename E>
struct EnumOption {
    E value;
    const char* label;
};

template <typename E>
struct ConfigEnumProps {
    Rml::String key;
    Rml::String helpText;
    std::vector<EnumOption<E>> options;
    std::function<void(E)> onChange;
    std::function<bool()> isDisabled;
    std::function<bool()> isModified;
    std::function<Rml::String()> unavailableReason;
    std::function<Rml::String(E)> optionUnavailableReason;
    std::function<Rml::String()> valueOverride;
    std::function<Rml::String()> helpSuffix;
};

struct ActionProps {
    Rml::String key;
    Rml::String helpText;
    std::function<Rml::String()> getValue;
    std::function<bool()> onPressed;
    std::function<bool()> isDisabled;
    std::function<Rml::String()> unavailableReason;
};

namespace detail {
std::function<bool()> locked_by(const std::function<Rml::String()>& unavailableReason);
std::function<bool()> unless_locked(std::function<bool()> locked, std::function<bool()> isDisabled);
}  // namespace detail

int float_setting_percent(ConfigVar<float>& var);

SelectButton& config_bool_select(
    Pane& leftPane, Pane& rightPane, ConfigVar<bool>& var, ConfigBoolProps props);

void add_speedrun_disabled_option(Pane& leftPane, Pane& rightPane, ConfigVar<bool>& var,
    const Rml::String& key, const Rml::String& helpText);

SelectButton& config_percent_select(Pane& leftPane, Pane& rightPane, ConfigVar<float>& var,
    Rml::String key, Rml::String helpText, int min, int max, int step = 5,
    std::function<bool()> isDisabled = {});

SelectButton& config_int_select(Pane& leftPane, Pane& rightPane, ConfigVar<int>& var,
    Rml::String key, Rml::String helpText, int min, int max, int step = 5,
    std::function<bool()> isDisabled = {}, std::function<void(int)> onChange = {},
    std::string suffix = "");

void graphics_tuner_control(
    Window& window, Pane& leftPane, Pane& rightPane, const GraphicsTunerProps& props);

SelectButton& config_action_select(Pane& leftPane, Pane& rightPane, ActionProps props);

Rml::String compose_help(const Rml::String& helpText, std::string_view reason);

template <typename E>
SelectButton& config_enum_select(
    Pane& leftPane, Pane& rightPane, ConfigVar<E>& var, ConfigEnumProps<E> props) {
    auto options = std::make_shared<const std::vector<EnumOption<E>>>(std::move(props.options));
    std::function<bool()> locked = detail::locked_by(props.unavailableReason);
    std::function<bool()> isModified = std::move(props.isModified);
    if (!isModified) {
        isModified = [&var] { return var.getValue() != var.getDefaultValue(); };
    }
    auto& button = leftPane.add_select_button({
        .key = std::move(props.key),
        .getValue =
            [&var, options, valueOverride = std::move(props.valueOverride)] {
                if (valueOverride) {
                    if (Rml::String value = valueOverride(); !value.empty()) {
                        return value;
                    }
                }
                for (const auto& option : *options) {
                    if (var.getValue() == option.value) {
                        return Rml::String{option.label};
                    }
                }
                return Rml::String{"Unknown"};
            },
        .isDisabled = detail::unless_locked(locked, std::move(props.isDisabled)),
        .isModified = std::move(isModified),
        .isLocked = locked,
    });
    leftPane.register_control(button, rightPane,
        [&var, options, helpText = std::move(props.helpText), onChange = std::move(props.onChange),
            unavailableReason = std::move(props.unavailableReason),
            optionUnavailableReason = std::move(props.optionUnavailableReason),
            helpSuffix = std::move(props.helpSuffix)](Pane& pane) {
            pane.clear();
            if (unavailableReason) {
                if (const Rml::String reason = unavailableReason(); !reason.empty()) {
                    pane.add_rml(compose_help(helpText, reason));
                    return;
                }
            }
            for (const auto& option : *options) {
                const E value = option.value;
                Rml::String text = option.label;
                if (optionUnavailableReason) {
                    if (const Rml::String reason = optionUnavailableReason(value); !reason.empty())
                    {
                        text += " (" + reason + ")";
                    }
                }
                pane.add_button({
                                    .text = std::move(text),
                                    .isSelected = [&var, value] { return var.getValue() == value; },
                                })
                    .on_pressed([&var, value, onChange] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        var.setValue(value);
                        config::save();
                        if (onChange) {
                            onChange(value);
                        }
                    });
            }
            pane.add_rml(compose_help(helpText, helpSuffix ? helpSuffix() : Rml::String{}));
        });
    return button;
}

}  // namespace dusk::ui
