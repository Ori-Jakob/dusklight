#pragma once

namespace twili::sync {

// Mutes the flag, item and save-table observers while received state is written into the save.
class RemoteApplyGuard {
public:
    RemoteApplyGuard() { enter(); }
    ~RemoteApplyGuard() { leave(); }
    RemoteApplyGuard(const RemoteApplyGuard&) = delete;
    RemoteApplyGuard& operator=(const RemoteApplyGuard&) = delete;

    static bool active() { return s_depth > 0; }
    // For spans that open and close in different hook callbacks.
    static void enter() { ++s_depth; }
    static void leave() {
        if (s_depth > 0) {
            --s_depth;
        }
    }
    static void reset() { s_depth = 0; }

private:
    static inline int s_depth = 0;
};

}  // namespace twili::sync
