#pragma once

// Archive copies loaded like dRes_control does, through the DVD command queue, never on the main
// thread. The JKR file/volume lists have no lock and some actor creates load synchronously on the
// main thread, so the request waits until no other actor is being created.

#include <dolphin/types.h>

class JKRArchive;
class JKRSolidHeap;
class mDoDvdThd_mountArchive_c;

namespace twili {

class PrivateArchives {
public:
    enum class State { Idle, Loading, Ready };

    static constexpr int kMax = 10;

    // Queues the mounts into a solid heap of heapSize from the game heap once the main thread is
    // quiet (or after a while); Idle while it waits, Loading once queued. Call every create phase.
    State update(const void* owner, const char* const* paths, int count, u32 heapSize);
    bool busy() const;
    bool failed() const { return mFailed; }
    JKRArchive* get(int i) const { return i >= 0 && i < mCount ? mArchives[i] : nullptr; }
    // Unmounts and frees the heap; a mount still in flight keeps (leaks) it.
    void release();

private:
    bool request(const char* const* paths, int count, u32 heapSize);
    State poll();

    mDoDvdThd_mountArchive_c* mCommands[kMax] = {};
    JKRArchive* mArchives[kMax] = {};
    JKRSolidHeap* mHeap = nullptr;
    int mCount = 0;
    int mWaited = 0;
    bool mFailed = false;
    State mState = State::Idle;
};

}  // namespace twili
