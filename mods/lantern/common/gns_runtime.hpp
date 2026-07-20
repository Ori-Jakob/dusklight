#pragma once

#include <string>

namespace lantern::net {

class GnsRuntime {
public:
    GnsRuntime() = default;
    GnsRuntime(const GnsRuntime&) = delete;
    GnsRuntime& operator=(const GnsRuntime&) = delete;
    ~GnsRuntime();

    bool initialize(std::string& error);
    void shutdown();
    bool initialized() const { return initialized_; }

private:
    bool initialized_ = false;
};

}  // namespace lantern::net
