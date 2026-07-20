#pragma once

#include "protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace lantern::test {

class LoopbackLink {
public:
    class Endpoint {
    public:
        bool send(
            std::vector<uint8_t> frame, protocol::Delivery delivery, std::string latest_key = {});
        bool receive(std::vector<uint8_t>& out);

    private:
        friend class LoopbackLink;
        Endpoint(LoopbackLink* owner, size_t side) : owner_(owner), side_(side) {}
        LoopbackLink* owner_;
        size_t side_;
    };

    LoopbackLink() : endpoints_{Endpoint(this, 0), Endpoint(this, 1)} {}
    Endpoint& first() { return endpoints_[0]; }
    Endpoint& second() { return endpoints_[1]; }
    void drop_next_unreliable() { drop_unreliable_ = true; }
    void reverse_pending(size_t destination);

private:
    struct Queued {
        protocol::Delivery delivery{};
        std::string latest_key;
        std::vector<uint8_t> frame;
    };
    bool send(size_t source, Queued queued);

    Endpoint endpoints_[2];
    std::deque<Queued> inbox_[2];
    bool drop_unreliable_ = false;
};

inline bool LoopbackLink::Endpoint::send(
    std::vector<uint8_t> frame, protocol::Delivery delivery, std::string latest_key) {
    return owner_->send(side_, {delivery, std::move(latest_key), std::move(frame)});
}

inline bool LoopbackLink::Endpoint::receive(std::vector<uint8_t>& out) {
    auto& inbox = owner_->inbox_[side_];
    if (inbox.empty())
        return false;
    out = std::move(inbox.front().frame);
    inbox.pop_front();
    return true;
}

inline bool LoopbackLink::send(size_t source, Queued queued) {
    if (queued.frame.empty() || queued.frame.size() > protocol::kMaxFrameBytes)
        return false;
    if (queued.delivery == protocol::Delivery::UnreliableLatest && drop_unreliable_) {
        drop_unreliable_ = false;
        return true;
    }
    auto& destination = inbox_[1 - source];
    if (queued.delivery == protocol::Delivery::UnreliableLatest && !queued.latest_key.empty()) {
        for (auto& existing : destination) {
            if (existing.delivery == protocol::Delivery::UnreliableLatest &&
                existing.latest_key == queued.latest_key)
            {
                existing = std::move(queued);
                return true;
            }
        }
    }
    if (destination.size() >= 256)
        return false;
    destination.push_back(std::move(queued));
    return true;
}

inline void LoopbackLink::reverse_pending(size_t destination) {
    if (destination > 1)
        return;
    std::reverse(inbox_[destination].begin(), inbox_[destination].end());
}

}  // namespace lantern::test
