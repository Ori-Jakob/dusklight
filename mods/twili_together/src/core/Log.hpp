#pragma once

#include <mods/svc/log.hpp>

#include <utility>

namespace twili {

// DuskLog-shaped logger over LogService; messages start with a tag such as "[net]".
struct Logger {
    template <typename... Args>
    void debug(fmt::format_string<Args...> format, Args&&... args) const {
        mods::log::debug(format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(fmt::format_string<Args...> format, Args&&... args) const {
        mods::log::info(format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(fmt::format_string<Args...> format, Args&&... args) const {
        mods::log::warn(format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(fmt::format_string<Args...> format, Args&&... args) const {
        mods::log::error(format, std::forward<Args>(args)...);
    }
};

inline constexpr Logger TwiliLog{};

}  // namespace twili
