#include "bool_button.hpp"

#include "Z2AudioLib/Z2SeMgr.h"
#include "m_Do/m_Do_audio.h"

namespace dusk::ui {

BoolButton::BoolButton(Rml::Element* parent, Props props)
    : BaseControlledSelectButton(parent,
          {
              .key = std::move(props.key),
              .icon = std::move(props.icon),
          }),
      mGetValue(std::move(props.getValue)), mSetValue(std::move(props.setValue)),
      mIsDisabled(std::move(props.isDisabled)), mIsModified(std::move(props.isModified)),
      mValueOverride(std::move(props.valueOverride)), mIsLocked(std::move(props.isLocked)) {}

bool BoolButton::modified() const {
    if (mIsModified) {
        return mIsModified();
    }
    return BaseControlledSelectButton::modified();
}

bool BoolButton::disabled() const {
    if (mIsDisabled) {
        return mIsDisabled();
    }
    return BaseControlledSelectButton::disabled();
}

bool BoolButton::locked() const {
    return mIsLocked && mIsLocked();
}

Rml::String BoolButton::format_value() {
    if (mValueOverride) {
        if (std::string value = mValueOverride(); !value.empty()) {
            return value;
        }
    }

    return mGetValue() ? "On" : "Off";
}

bool BoolButton::handle_nav_command(NavCommand cmd) {
    if (locked() &&
        (cmd == NavCommand::Confirm || cmd == NavCommand::Left || cmd == NavCommand::Right))
    {
        mDoAud_seStartMenu(kSoundWarning);
        return true;
    }
    if (cmd == NavCommand::Confirm || cmd == NavCommand::Left || cmd == NavCommand::Right) {
        const bool newValue = !mGetValue();
        mSetValue(newValue);
        mDoAud_seStartMenu(newValue ? kSoundItemEnable : kSoundItemDisable);
        return true;
    }
    return false;
}

}  // namespace dusk::ui