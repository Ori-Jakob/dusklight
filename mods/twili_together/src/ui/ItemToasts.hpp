#pragma once

// Toasts for teammates' pickups; replays and catch-ups merge into one summary per window.

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace twili {
class Session;
}

namespace twili::item_toasts {

struct Sender {
    uint32_t clientId = 0;
    std::string name;  // "" = unknown ("A teammate")
    uint8_t r = 255, g = 255, b = 255;
    bool hasColor = false;
    bool nameFromPacket = false;  // from the server's stamp, not the live roster
    bool self = false;
};

enum class Source {
    Live,          // a teammate's pickup, as it happens
    Replay,        // the server's queue, replayed on join or reconnect (fromQueue)
    CatchUpMerge,  // the answer to our catch-up request, or the server's cached state
    LiveMerge,     // a teammate's broadcast world state
};

struct ItemCount {
    uint8_t itemNo = 0;
    int count = 1;
    int saveTbl = -1;  // the stage table of a dungeon item, -1 otherwise
};

// A team packet's sender: its roster row, else the server's senderName/senderColor stamp.
Sender senderOf(const Session& session, const nlohmann::json& packet);

// Progression items plus the small key, map, compass and boss keys.
bool isToastableItem(uint8_t itemNo);

// A live one always toasts; a replayed one counts into the summary only if `applied`.
void noteItem(const Sender& who, uint8_t itemNo, int count, int saveTbl, Source src, bool applied);
// Items a world-state merge made new to us (CatchUpMerge or LiveMerge).
void noteMerge(const Sender& who, Source src, const std::vector<ItemCount>& items);
// Our own pickup (ItemService give with origin GAME), shown only with item_toasts_own.
void noteOwnItem(uint8_t itemNo, int saveTbl);
// Our catch-up request went out: replays and merges in the next seconds are summarized.
void beginCatchUp();
// Once per tick, connected or not.
void tick();
// Pending toasts and the open window are dropped, history is kept.
void resetSession();

// Autotest readback.
struct Record {
    uint32_t seq = 0;
    std::string kind;  // "item", "own", "summary" (catch-up) or "merged" (backlog)
    uint32_t senderId = 0;
    std::vector<std::string> senderNames;  // as shown; several for a summary, "self" for us
    bool nameFromPacket = false;
    std::vector<uint8_t> items;  // the item for "item"/"own"; the counted items otherwise
    int count = 0;               // xN of an item, the total of a summary
    std::string text;            // plain text as shown
    std::string rml;             // the body sent to the overlay
    bool iconOk = false;         // the game files have an icon for the first item
    uint32_t iconChecksum = 0;
    bool pushed = false;
    bool mergedAway = false;  // folded into a later "merged" record before it was shown
    std::string holdReason;   // why it waits, "" once pushed
};
const std::deque<Record>& history();
// The item's icon as the game files have it: layers, the first one's size, a texel checksum.
struct IconProbe {
    bool ok = false;
    int layers = 0;
    int width = 0;
    int height = 0;
    uint32_t checksum = 0;
};
IconProbe iconProbe(uint8_t itemNo);
void clearIconCache();
size_t pendingCount();
void clearForTest();

}  // namespace twili::item_toasts
