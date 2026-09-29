#include "actors/PrivateArchives.hpp"
#include "core/GameAccess.hpp"
#include "core/Log.hpp"

#include "f_pc/f_pc_base.h"
#include "f_pc/f_pc_create_req.h"
#include "f_pc/f_pc_leaf.h"
#include "JSystem/JKernel/JKRMemArchive.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "m_Do/m_Do_dvd_thread.h"
#include "m_Do/m_Do_ext.h"
#include "SSystem/SComponent/c_node.h"

namespace twili {
namespace {

// About 10 s of create phases; then load anyway rather than never showing the puppet.
constexpr int kMaxQuietWait = 300;

// Another actor (leaf process) still in its create phase, not one of our puppets. Scenes stay in
// the queue while the stage runs, so they do not count.
bool othersCreating(const void* owner) {
    for (node_class* node = g_fpcCtTg_Queue.mpHead; node != nullptr; node = NODE_GET_NEXT(node)) {
        const auto* request =
            static_cast<const create_request*>(reinterpret_cast<create_tag_class*>(node)->mpTagData);
        const base_process_class* process = request != nullptr ? request->process : nullptr;
        if (process == nullptr || process == owner ||
            !fpcBs_Is_JustOfType(g_fpcLf_type, process->subtype))
        {
            continue;
        }
        if (process->profname != g_procDummyPlayer && process->profname != g_procDummyHorse) {
            return true;
        }
    }
    return false;
}

}  // namespace

PrivateArchives::State PrivateArchives::update(const void* owner, const char* const* paths,
                                               int count, u32 heapSize) {
    if (mState == State::Idle && !mFailed) {
        if (othersCreating(owner) && mWaited++ < kMaxQuietWait) {
            return mState;
        }
        mFailed = !request(paths, count, heapSize);
        return mState;
    }
    return poll();
}

bool PrivateArchives::request(const char* const* paths, int count, u32 heapSize) {
    if (count <= 0 || count > kMax) {
        return false;
    }
    mHeap = mDoExt_createSolidHeapFromGame(heapSize, 0x20);
    if (mHeap == nullptr) {
        return false;
    }
    mCount = count;
    for (int i = 0; i < count; i++) {
        // Direction 0 is the head, so the heap can be trimmed afterwards.
        mCommands[i] = mDoDvdThd_mountArchive_c::create(paths[i], 0, mHeap);
    }
    mState = State::Loading;
    return true;
}

bool PrivateArchives::busy() const {
    for (int i = 0; i < mCount; i++) {
        if (mCommands[i] != nullptr && !mCommands[i]->sync()) {
            return true;
        }
    }
    return false;
}

PrivateArchives::State PrivateArchives::poll() {
    if (mState != State::Loading || busy()) {
        return mState;
    }
    for (int i = 0; i < mCount; i++) {
        if (mCommands[i] != nullptr) {
            mArchives[i] = mCommands[i]->getArchive();
            mCommands[i]->destroy();
            mCommands[i] = nullptr;
        }
    }
    mDoExt_adjustSolidHeap(mHeap);
    mState = State::Ready;
    return mState;
}

void PrivateArchives::release() {
    if (mState != State::Idle) {
        if (busy()) {
            // Only a forced delete (mod unload) gets here; the loader still writes into mHeap.
            TwiliLog.warn("[archives] deleted while loading, leaving the heap to the loader");
        } else {
            poll();
            // Unmounted here like dRes_info_c does when a stage's resources go.
            for (int i = 0; i < mCount; i++) {
                if (mArchives[i] != nullptr) {
                    mArchives[i]->unmount();
                }
            }
            mDoExt_destroySolidHeap(mHeap);
        }
    }
    *this = PrivateArchives{};
}

}  // namespace twili
