#include "story/StoryLog.hpp"

#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "game/GameIdentity.hpp"
#include "game/RandoSeeds.hpp"

#include <mods/svc/host.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace twili::story::storylog {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr size_t kMaxRecent = 32;
constexpr size_t kMaxLearned = 256;
constexpr auto kSaveDelay = std::chrono::seconds(2);

// File reads and writes stay off the game thread.
class Worker {
public:
    void post(std::function<void()> job) {
        {
            std::lock_guard lock(mMutex);
            if (!mThread.joinable()) {
                mStop = false;
                mThread = std::thread([this] { run(); });
            }
            mJobs.push_back(std::move(job));
        }
        mCv.notify_one();
    }
    void stop() {
        {
            std::lock_guard lock(mMutex);
            mStop = true;
        }
        mCv.notify_one();
        if (mThread.joinable()) {
            mThread.join();
        }
        mJobs.clear();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mMutex);
                mCv.wait(lock, [this] { return mStop || !mJobs.empty(); });
                if (mJobs.empty()) {
                    return;
                }
                job = std::move(mJobs.front());
                mJobs.pop_front();
            }
            job();
        }
    }

    std::mutex mMutex;
    std::condition_variable mCv;
    std::deque<std::function<void()>> mJobs;
    std::thread mThread;
    bool mStop = false;
};

struct Loaded {
    std::string identity;
    std::map<std::string, Learned> entries;
    int64_t lastOwnMs = 0;
};

Worker s_worker;
std::deque<MoveRecord> s_recent;
std::map<std::string, Learned> s_learned;
std::string s_identity;
fs::path s_path;
bool s_dirty = false;
int64_t s_lastOwnMs = 0;
Clock::time_point s_saveAt{};
std::mutex s_loadedMutex;
std::optional<Loaded> s_loaded;

fs::path learnedDir() {
    const char* dir = nullptr;
    if (!SERVICE_HAS(svc_host, HostService, data_dir) ||
        svc_host->data_dir(mod_ctx, &dir) != MOD_OK || dir == nullptr)
    {
        return {};
    }
    return fs::path(reinterpret_cast<const char8_t*>(dir)) / "story-learned";
}

nlohmann::json entryJson(const Learned& e) {
    return {{"move", e.move.toJson()}, {"seen", e.seen}, {"lastSeen", e.lastSeenMs},
        {"own", e.own}};
}

std::map<std::string, Learned> parseFile(const std::string& text, int64_t& lastOwnMs) {
    std::map<std::string, Learned> out;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("entries") || !j["entries"].is_array()) {
        return out;
    }
    if (const auto it = j.find("lastOwn"); it != j.end() && it->is_number_integer()) {
        lastOwnMs = it->get<int64_t>();
    }
    for (const auto& item : j["entries"]) {
        if (!item.is_object() || !item.contains("move") || !item["move"].is_object()) {
            continue;
        }
        Learned e;
        try {
            e.move = MoveRecord::fromJson(item["move"]);
            e.seen = item.value("seen", 1u);
            e.lastSeenMs = item.value("lastSeen", int64_t{0});
            e.own = item.value("own", false);
        } catch (const nlohmann::json::exception&) {
            continue;
        }
        if (!e.move.key.empty() && e.move.to.valid()) {
            out[e.move.key] = std::move(e);
        }
    }
    return out;
}

void trimLearned() {
    while (s_learned.size() > kMaxLearned) {
        auto oldest = s_learned.begin();
        for (auto it = s_learned.begin(); it != s_learned.end(); ++it) {
            if (it->second.lastSeenMs < oldest->second.lastSeenMs) {
                oldest = it;
            }
        }
        s_learned.erase(oldest);
    }
}

void saveNow() {
    s_dirty = false;
    if (s_path.empty()) {
        return;
    }
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& [key, e] : s_learned) {
        entries.push_back(entryJson(e));
    }
    const nlohmann::json j = {{"version", 1}, {"identity", s_identity}, {"lastOwn", s_lastOwnMs},
        {"entries", std::move(entries)}};
    std::string text = j.dump(1);
    s_worker.post([path = s_path, text = std::move(text)] {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        const fs::path tmp = fs::path(path).concat(".tmp");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << text;
            if (!out) {
                return;
            }
        }
        fs::rename(tmp, path, ec);
    });
}

void switchIdentity(const std::string& identity) {
    if (s_dirty) {
        saveNow();
    }
    s_identity = identity;
    s_learned.clear();
    s_lastOwnMs = 0;
    s_path.clear();
    if (identity.empty()) {
        return;
    }
    const fs::path dir = learnedDir();
    if (dir.empty()) {
        return;
    }
    s_path = dir / (rando_seeds::fnv64Hex(identity) + ".json");
    s_worker.post([path = s_path, identity] {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        if (in) {
            text << in.rdbuf();
        }
        Loaded loaded;
        loaded.identity = identity;
        if (in) {
            loaded.entries = parseFile(text.str(), loaded.lastOwnMs);
        }
        std::lock_guard lock(s_loadedMutex);
        s_loaded = std::move(loaded);
    });
}

}  // namespace

int64_t unixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void note(const MoveRecord& m, bool own) {
    if (m.key.empty() || m.qual == 0 || !m.to.valid()) {
        return;
    }
    for (auto it = s_recent.begin(); it != s_recent.end(); ++it) {
        if (it->key == m.key) {
            s_recent.erase(it);
            break;
        }
    }
    s_recent.push_back(m);
    while (s_recent.size() > kMaxRecent) {
        s_recent.pop_front();
    }
    if (s_identity.empty() || !local::isKnownEntrance(m.to.stage, m.to.room, m.to.point)) {
        return;
    }
    Learned& e = s_learned[m.key];
    e.move = m;
    e.seen++;
    e.lastSeenMs = unixMs();
    e.own = own;
    trimLearned();
    TwiliLog.info("[story] learned '{}' -> {} room {} point {} layer {} (seen {})", m.key,
        m.to.stage, m.to.room, m.to.point, m.to.layer, e.seen);
    if (!s_dirty) {
        s_dirty = true;
        s_saveAt = Clock::now() + kSaveDelay;
    }
}

const std::deque<MoveRecord>& recent() {
    return s_recent;
}

const Learned* learnedByKey(const std::string& key) {
    const auto it = s_learned.find(key);
    return it != s_learned.end() ? &it->second : nullptr;
}

const Learned* learnedByCurated(const char* curatedId) {
    const Learned* best = nullptr;
    for (const auto& [key, e] : s_learned) {
        const StoryMoveDef* def = curatedMove(e.move.curated);
        if (def != nullptr && std::string_view(def->id) == curatedId &&
            (best == nullptr || e.lastSeenMs > best->lastSeenMs))
        {
            best = &e;
        }
    }
    return best;
}

size_t learnedCount() {
    return s_learned.size();
}

const std::map<std::string, Learned>& learned() {
    return s_learned;
}

void noteOwnStoryArrival() {
    s_lastOwnMs = unixMs();
    if (!s_identity.empty() && !s_dirty) {
        s_dirty = true;
        s_saveAt = Clock::now() + kSaveDelay;
    }
}

int64_t lastOwnStoryMs() {
    return s_lastOwnMs;
}

void forget() {
    s_recent.clear();
    s_learned.clear();
    TwiliLog.info("[story] learned story forgotten for this game");
    saveNow();
}

void tick() {
    const game_identity::Identity& id = game_identity::current();
    const std::string identity = id.inGame ? id.key : std::string{};
    if (identity != s_identity && !identity.empty()) {
        switchIdentity(identity);
    }
    {
        std::lock_guard lock(s_loadedMutex);
        if (s_loaded && s_loaded->identity == s_identity) {
            // What we learned meanwhile is newer than the file.
            for (auto& [key, e] : s_loaded->entries) {
                s_learned.try_emplace(key, std::move(e));
            }
            s_lastOwnMs = std::max(s_lastOwnMs, s_loaded->lastOwnMs);
            trimLearned();
            TwiliLog.info("[story] {} learned move(s) for this game", s_learned.size());
        }
        s_loaded.reset();
    }
    if (s_dirty && Clock::now() >= s_saveAt) {
        saveNow();
    }
}

void shutdown() {
    if (s_dirty) {
        saveNow();
    }
    s_worker.stop();
    s_recent.clear();
    s_learned.clear();
    s_identity.clear();
    s_path.clear();
    s_lastOwnMs = 0;
    std::lock_guard lock(s_loadedMutex);
    s_loaded.reset();
}

}  // namespace twili::story::storylog
