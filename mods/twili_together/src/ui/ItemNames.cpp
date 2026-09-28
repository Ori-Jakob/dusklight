#include "ui/ItemNames.hpp"

#include "JSystem/JKernel/JKRArchive.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_stage.h"

#include <fmt/format.h>

#include <array>
#include <cctype>
#include <cstring>
#include <utility>

namespace twili::ui {
namespace {

// Item n's name is message n + 0x165 (the collection screen and the item ring).
constexpr uint32_t kItemNameMessageBase = 0x165;
constexpr uint32_t kSnowpeakMapMessage = 0x5BF;
constexpr size_t kMaxNameBytes = 128;

constexpr std::pair<uint8_t, const char*> kEnglishNames[] = {
    {dItemNo_HEART_e, "Heart"},
    {dItemNo_GREEN_RUPEE_e, "Green Rupee"},
    {dItemNo_BLUE_RUPEE_e, "Blue Rupee"},
    {dItemNo_YELLOW_RUPEE_e, "Yellow Rupee"},
    {dItemNo_RED_RUPEE_e, "Red Rupee"},
    {dItemNo_PURPLE_RUPEE_e, "Purple Rupee"},
    {dItemNo_ORANGE_RUPEE_e, "Orange Rupee"},
    {dItemNo_SILVER_RUPEE_e, "Silver Rupee"},
    {dItemNo_S_MAGIC_e, "Small Magic"},
    {dItemNo_L_MAGIC_e, "Large Magic"},
    {dItemNo_BOMB_5_e, "Bombs (5)"},
    {dItemNo_BOMB_10_e, "Bombs (10)"},
    {dItemNo_BOMB_20_e, "Bombs (20)"},
    {dItemNo_BOMB_30_e, "Bombs (30)"},
    {dItemNo_ARROW_10_e, "Arrows (10)"},
    {dItemNo_ARROW_20_e, "Arrows (20)"},
    {dItemNo_ARROW_30_e, "Arrows (30)"},
    {dItemNo_ARROW_1_e, "Arrows (1)"},
    {dItemNo_PACHINKO_SHOT_e, "Pumpkin Seeds"},
    {dItemNo_NOENTRY_19_e, "Reserved"},
    {dItemNo_NOENTRY_20_e, "Reserved"},
    {dItemNo_NOENTRY_21_e, "Reserved"},
    {dItemNo_WATER_BOMB_5_e, "Water Bombs (5)"},
    {dItemNo_WATER_BOMB_10_e, "Water Bombs (10)"},
    {dItemNo_WATER_BOMB_20_e, "Water Bombs (20)"},
    {dItemNo_WATER_BOMB_30_e, "Water Bombs (30)"},
    {dItemNo_BOMB_INSECT_5_e, "Bomblings (5)"},
    {dItemNo_BOMB_INSECT_10_e, "Bomblings (10)"},
    {dItemNo_BOMB_INSECT_20_e, "Bomblings (20)"},
    {dItemNo_BOMB_INSECT_30_e, "Bomblings (30)"},
    {dItemNo_RECOVERY_FAILY_e, "Fairy"},
    {dItemNo_TRIPLE_HEART_e, "Triple Hearts"},
    {dItemNo_SMALL_KEY_e, "Small Key"},
    {dItemNo_KAKERA_HEART_e, "Piece of Heart"},
    {dItemNo_UTAWA_HEART_e, "Heart Container"},
    {dItemNo_MAP_e, "Dungeon Map"},
    {dItemNo_COMPUS_e, "Compass"},
    {dItemNo_DUNGEON_EXIT_e, "Ooccoo Sr. (First Time)"},
    {dItemNo_BOSS_KEY_e, "Boss Key"},
    {dItemNo_DUNGEON_BACK_e, "Ooccoo Jr."},
    {dItemNo_SWORD_e, "Ordon Sword"},
    {dItemNo_MASTER_SWORD_e, "Master Sword"},
    {dItemNo_WOOD_SHIELD_e, "Ordon Shield"},
    {dItemNo_SHIELD_e, "Wooden Shield"},
    {dItemNo_HYLIA_SHIELD_e, "Hylian Shield"},
    {dItemNo_TKS_LETTER_e, "Ooccoo's Note"},
    {dItemNo_WEAR_CASUAL_e, "Ordon Clothes"},
    {dItemNo_WEAR_KOKIRI_e, "Hero's Clothes"},
    {dItemNo_ARMOR_e, "Magic Armor"},
    {dItemNo_WEAR_ZORA_e, "Zora Armor"},
    {dItemNo_SHADOW_CRYSTAL_e, "Shadow Crystal"},
    {dItemNo_DUNGEON_EXIT_2_e, "Ooccoo Sr."},
    {dItemNo_WALLET_LV1_e, "Wallet"},
    {dItemNo_WALLET_LV2_e, "Big Wallet"},
    {dItemNo_WALLET_LV3_e, "Giant Wallet"},
    {dItemNo_NOENTRY_55_e, "Reserved"},
    {dItemNo_NOENTRY_56_e, "Reserved"},
    {dItemNo_NOENTRY_57_e, "Reserved"},
    {dItemNo_NOENTRY_58_e, "Reserved"},
    {dItemNo_NOENTRY_59_e, "Reserved"},
    {dItemNo_NOENTRY_60_e, "Reserved"},
    {dItemNo_ZORAS_JEWEL_e, "Coral Earring"},
    {dItemNo_HAWK_EYE_e, "Hawkeye"},
    {dItemNo_WOOD_STICK_e, "Wooden Sword"},
    {dItemNo_BOOMERANG_e, "Gale Boomerang"},
    {dItemNo_SPINNER_e, "Spinner"},
    {dItemNo_IRONBALL_e, "Ball and Chain"},
    {dItemNo_BOW_e, "Hero's Bow"},
    {dItemNo_HOOKSHOT_e, "Clawshot"},
    {dItemNo_HVY_BOOTS_e, "Iron Boots"},
    {dItemNo_COPY_ROD_e, "Dominion Rod"},
    {dItemNo_W_HOOKSHOT_e, "Double Clawshots"},
    {dItemNo_KANTERA_e, "Lantern"},
    {dItemNo_LIGHT_SWORD_e, "Light Sword"},
    {dItemNo_FISHING_ROD_1_e, "Fishing Rod"},
    {dItemNo_PACHINKO_e, "Slingshot"},
    {dItemNo_COPY_ROD_2_e, "Dominion Rod (Uncharged)"},
    {dItemNo_NOENTRY_77_e, "Reserved"},
    {dItemNo_NOENTRY_78_e, "Reserved"},
    {dItemNo_BOMB_BAG_LV2_e, "Giant Bomb Bag"},
    {dItemNo_BOMB_BAG_LV1_e, "Empty Bomb Bag"},
    {dItemNo_BOMB_IN_BAG_e, "Bomb Bag"},
    {dItemNo_NOENTRY_82_e, "Reserved"},
    {dItemNo_LIGHT_ARROW_e, "Light Arrow"},
    {dItemNo_ARROW_LV1_e, "Quiver"},
    {dItemNo_ARROW_LV2_e, "Big Quiver"},
    {dItemNo_ARROW_LV3_e, "Giant Quiver"},
    {dItemNo_NOENTRY_87_e, "Reserved"},
    {dItemNo_LURE_ROD_e, "Fishing Rod (Lure)"},
    {dItemNo_BOMB_ARROW_e, "Bomb Arrow"},
    {dItemNo_HAWK_ARROW_e, "Hawk Arrow"},
    {dItemNo_BEE_ROD_e, "Fishing Rod (Bee Larva)"},
    {dItemNo_JEWEL_ROD_e, "Fishing Rod (Earring)"},
    {dItemNo_WORM_ROD_e, "Fishing Rod (Worm)"},
    {dItemNo_JEWEL_BEE_ROD_e, "Fishing Rod (Earring + Bee Larva)"},
    {dItemNo_JEWEL_WORM_ROD_e, "Fishing Rod (Earring + Worm)"},
    {dItemNo_EMPTY_BOTTLE_e, "Empty Bottle"},
    {dItemNo_RED_BOTTLE_e, "Red Potion"},
    {dItemNo_GREEN_BOTTLE_e, "Green Potion"},
    {dItemNo_BLUE_BOTTLE_e, "Blue Potion"},
    {dItemNo_MILK_BOTTLE_e, "Milk Bottle"},
    {dItemNo_HALF_MILK_BOTTLE_e, "Half Milk Bottle"},
    {dItemNo_OIL_BOTTLE_e, "Lantern Oil"},
    {dItemNo_WATER_BOTTLE_e, "Water Bottle"},
    {dItemNo_OIL_BOTTLE_2_e, "Lantern Oil (Scooped)"},
    {dItemNo_RED_BOTTLE_2_e, "Red Potion (Scooped)"},
    {dItemNo_UGLY_SOUP_e, "Nasty Soup"},
    {dItemNo_HOT_SPRING_e, "Hotspring Water"},
    {dItemNo_FAIRY_e, "Fairy"},
    {dItemNo_HOT_SPRING_2_e, "Hotspring Water (Shop)"},
    {dItemNo_OIL2_e, "Lantern Refill (Scooped)"},
    {dItemNo_OIL_e, "Lantern Refill (Shop)"},
    {dItemNo_NORMAL_BOMB_e, "Bombs"},
    {dItemNo_WATER_BOMB_e, "Water Bombs"},
    {dItemNo_POKE_BOMB_e, "Bomblings"},
    {dItemNo_FAIRY_DROP_e, "Great Fairy's Tears"},
    {dItemNo_WORM_e, "Worm"},
    {dItemNo_DROP_BOTTLE_e, "Great Fairy Tears (Jovani)"},
    {dItemNo_BEE_CHILD_e, "Bee Larva"},
    {dItemNo_CHUCHU_RARE_e, "Rare Chu Jelly"},
    {dItemNo_CHUCHU_RED_e, "Red Chu Jelly"},
    {dItemNo_CHUCHU_BLUE_e, "Blue Chu Jelly"},
    {dItemNo_CHUCHU_GREEN_e, "Green Chu Jelly"},
    {dItemNo_CHUCHU_YELLOW_e, "Yellow Chu Jelly"},
    {dItemNo_CHUCHU_PURPLE_e, "Purple Chu Jelly"},
    {dItemNo_LV1_SOUP_e, "Simple Soup"},
    {dItemNo_LV2_SOUP_e, "Good Soup"},
    {dItemNo_LV3_SOUP_e, "Superb Soup"},
    {dItemNo_LETTER_e, "Renado's Letter"},
    {dItemNo_BILL_e, "Invoice"},
    {dItemNo_WOOD_STATUE_e, "Wooden Statue"},
    {dItemNo_IRIAS_PENDANT_e, "Ilia's Charm"},
    {dItemNo_HORSE_FLUTE_e, "Horse Call"},
    {dItemNo_NOENTRY_133_e, "Reserved"},
    {dItemNo_NOENTRY_134_e, "Reserved"},
    {dItemNo_NOENTRY_135_e, "Reserved"},
    {dItemNo_NOENTRY_136_e, "Reserved"},
    {dItemNo_NOENTRY_137_e, "Reserved"},
    {dItemNo_NOENTRY_138_e, "Reserved"},
    {dItemNo_NOENTRY_139_e, "Reserved"},
    {dItemNo_NOENTRY_140_e, "Reserved"},
    {dItemNo_NOENTRY_141_e, "Reserved"},
    {dItemNo_NOENTRY_142_e, "Reserved"},
    {dItemNo_NOENTRY_143_e, "Reserved"},
    {dItemNo_RAFRELS_MEMO_e, "Auru's Memo"},
    {dItemNo_ASHS_SCRIBBLING_e, "Ashei's Sketch"},
    {dItemNo_NOENTRY_146_e, "Reserved"},
    {dItemNo_NOENTRY_147_e, "Reserved"},
    {dItemNo_NOENTRY_148_e, "Reserved"},
    {dItemNo_NOENTRY_149_e, "Reserved"},
    {dItemNo_NOENTRY_150_e, "Reserved"},
    {dItemNo_NOENTRY_151_e, "Reserved"},
    {dItemNo_NOENTRY_152_e, "Reserved"},
    {dItemNo_NOENTRY_153_e, "Reserved"},
    {dItemNo_NOENTRY_154_e, "Reserved"},
    {dItemNo_NOENTRY_155_e, "Reserved"},
    {dItemNo_CHUCHU_YELLOW2_e, "Lantern Refill (Yellow Chu)"},
    {dItemNo_OIL_BOTTLE3_e, "Lantern Oil (Coro)"},
    {dItemNo_SHOP_BEE_CHILD_e, "Bee Larve (Shop)"},
    {dItemNo_CHUCHU_BLACK_e, "Black Chu Jelly"},
    {dItemNo_LIGHT_DROP_e, "Tear of Light"},
    {dItemNo_DROP_CONTAINER_e, "Vessel of Light (Faron)"},
    {dItemNo_DROP_CONTAINER02_e, "Vessel of Light (Eldin)"},
    {dItemNo_DROP_CONTAINER03_e, "Vessel of Light (Lanayru)"},
    {dItemNo_FILLED_CONTAINER_e, "Vessel of Light (Filled)"},
    {dItemNo_MIRROR_PIECE_2_e, "Mirror Shard (Snowpeak Ruins)"},
    {dItemNo_MIRROR_PIECE_3_e, "Mirror Shard (Temple of Time)"},
    {dItemNo_MIRROR_PIECE_4_e, "Mirror Shard (City in the Sky)"},
    {dItemNo_NOENTRY_168_e, "Reserved"},
    {dItemNo_NOENTRY_169_e, "Reserved"},
    {dItemNo_NOENTRY_170_e, "Reserved"},
    {dItemNo_NOENTRY_171_e, "Reserved"},
    {dItemNo_NOENTRY_172_e, "Reserved"},
    {dItemNo_NOENTRY_173_e, "Reserved"},
    {dItemNo_NOENTRY_174_e, "Reserved"},
    {dItemNo_NOENTRY_175_e, "Reserved"},
    {dItemNo_SMELL_YELIA_POUCH_e, "Scent of Ilia"},
    {dItemNo_SMELL_PUMPKIN_e, "Pumpkin Scent"},
    {dItemNo_SMELL_POH_e, "Poe Scent"},
    {dItemNo_SMELL_FISH_e, "Reekfish Scent"},
    {dItemNo_SMELL_CHILDREN_e, "Youth's Scent"},
    {dItemNo_SMELL_MEDICINE_e, "Medicine Scent"},
    {dItemNo_NOENTRY_182_e, "Reserved"},
    {dItemNo_NOENTRY_183_e, "Reserved"},
    {dItemNo_NOENTRY_184_e, "Reserved"},
    {dItemNo_NOENTRY_185_e, "Reserved"},
    {dItemNo_NOENTRY_186_e, "Reserved"},
    {dItemNo_NOENTRY_187_e, "Reserved"},
    {dItemNo_NOENTRY_188_e, "Reserved"},
    {dItemNo_NOENTRY_189_e, "Reserved"},
    {dItemNo_NOENTRY_190_e, "Reserved"},
    {dItemNo_NOENTRY_191_e, "Reserved"},
    {dItemNo_M_BEETLE_e, "Beetle (M)"},
    {dItemNo_F_BEETLE_e, "Beetle (F)"},
    {dItemNo_M_BUTTERFLY_e, "Butterfly (M)"},
    {dItemNo_F_BUTTERFLY_e, "Butterfly (F)"},
    {dItemNo_M_STAG_BEETLE_e, "Stag Beetle (M)"},
    {dItemNo_F_STAG_BEETLE_e, "Stag Beetle (F)"},
    {dItemNo_M_GRASSHOPPER_e, "Grasshopper (M)"},
    {dItemNo_F_GRASSHOPPER_e, "Grasshopper (F)"},
    {dItemNo_M_NANAFUSHI_e, "Phasmid (M)"},
    {dItemNo_F_NANAFUSHI_e, "Phasmid (F)"},
    {dItemNo_M_DANGOMUSHI_e, "Pill Bug (M)"},
    {dItemNo_F_DANGOMUSHI_e, "Pill Bug (F)"},
    {dItemNo_M_MANTIS_e, "Mantis (M)"},
    {dItemNo_F_MANTIS_e, "Mantis (F)"},
    {dItemNo_M_LADYBUG_e, "Ladybug (M)"},
    {dItemNo_F_LADYBUG_e, "Ladybug (F)"},
    {dItemNo_M_SNAIL_e, "Snail (M)"},
    {dItemNo_F_SNAIL_e, "Snail (F)"},
    {dItemNo_M_DRAGONFLY_e, "Dragonfly (M)"},
    {dItemNo_F_DRAGONFLY_e, "Dragonfly (F)"},
    {dItemNo_M_ANT_e, "Ant (M)"},
    {dItemNo_F_ANT_e, "Ant (F)"},
    {dItemNo_M_MAYFLY_e, "Mayfly (M)"},
    {dItemNo_F_MAYFLY_e, "Mayfly (F)"},
    {dItemNo_NOENTRY_216_e, "Reserved"},
    {dItemNo_NOENTRY_217_e, "Reserved"},
    {dItemNo_NOENTRY_218_e, "Reserved"},
    {dItemNo_NOENTRY_219_e, "Reserved"},
    {dItemNo_NOENTRY_220_e, "Reserved"},
    {dItemNo_NOENTRY_221_e, "Reserved"},
    {dItemNo_NOENTRY_222_e, "Reserved"},
    {dItemNo_NOENTRY_223_e, "Reserved"},
    {dItemNo_POU_SPIRIT_e, "Poe Soul"},
    {dItemNo_NOENTRY_225_e, "Reserved"},
    {dItemNo_NOENTRY_226_e, "Reserved"},
    {dItemNo_NOENTRY_227_e, "Reserved"},
    {dItemNo_NOENTRY_228_e, "Reserved"},
    {dItemNo_NOENTRY_229_e, "Reserved"},
    {dItemNo_NOENTRY_230_e, "Reserved"},
    {dItemNo_NOENTRY_231_e, "Reserved"},
    {dItemNo_NOENTRY_232_e, "Reserved"},
    {dItemNo_ANCIENT_DOCUMENT_e, "Ancient Sky Book"},
    {dItemNo_AIR_LETTER_e, "Ancient Sky Book (Partial)"},
    {dItemNo_ANCIENT_DOCUMENT2_e, "Ancient Sky Book (Filled)"},
    {dItemNo_LV7_DUNGEON_EXIT_e, "Ooccoo Sr. (City in the Sky)"},
    {dItemNo_LINKS_SAVINGS_e, "Purple Rupee (Link's Savings)"},
    {dItemNo_SMALL_KEY2_e, "Small Key (North Faron Gate)"},
    {dItemNo_POU_FIRE1_e, "Poe Fire 1"},
    {dItemNo_POU_FIRE2_e, "Poe Fire 2"},
    {dItemNo_POU_FIRE3_e, "Poe Fire 3"},
    {dItemNo_POU_FIRE4_e, "Poe Fire 4"},
    {dItemNo_BOSSRIDER_KEY_e, "Hyrule Field Keys"},
    {dItemNo_TOMATO_PUREE_e, "Ordon Pumpkin"},
    {dItemNo_TASTE_e, "Ordon Goat Cheese"},
    {dItemNo_LV5_BOSS_KEY_e, "Bedroom Key"},
    {dItemNo_SURFBOARD_e, "Surf Leaf"},
    {dItemNo_KANTERA2_e, "Lantern (Reclaimed)"},
    {dItemNo_L2_KEY_PIECES1_e, "Key Shard (1)"},
    {dItemNo_L2_KEY_PIECES2_e, "Key Shard (2)"},
    {dItemNo_L2_KEY_PIECES3_e, "Key Shard (3)"},
    {dItemNo_KEY_OF_CARAVAN_e, "Bulblin Camp Key"},
    {dItemNo_LV2_BOSS_KEY_e, "Goron Mines Boss Key"},
    {dItemNo_KEY_OF_FILONE_e, "South Faron Gate Key"},
    {dItemNo_NONE_e, "None"},
};

struct NameEntry {
    bool known = false;
    bool fromGame = false;
    std::string text;
};

std::array<NameEntry, 256> sNames;
NameEntry sSnowpeakMap;

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

uint32_t be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6);
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | cp >> 12);
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | cp >> 18);
        out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Windows-1252 0x80-0x9F; 0 where the code page leaves a byte undefined.
constexpr uint16_t kCp1252High[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

// Newlines become spaces, control characters go, runs of spaces collapse.
void appendCodePoint(std::string& out, uint32_t cp) {
    if (cp == '\n') {
        cp = ' ';
    }
    if (cp < 0x20 || cp == 0x7F) {
        return;
    }
    if (cp == ' ' && (out.empty() || out.back() == ' ')) {
        return;
    }
    if (out.size() < kMaxNameBytes) {
        appendUtf8(out, cp);
    }
}

// One message as UTF-8 with JMessage tags skipped; "" for Shift-JIS and anything malformed.
std::string decodeMessage(const uint8_t* p, const uint8_t* end, uint8_t encoding) {
    std::string out;
    if (encoding == 3) {
        return out;
    }
    if (encoding == 2) {  // UTF-16BE
        while (end - p >= 2) {
            const uint16_t unit = be16(p);
            if (unit == 0) {
                break;
            }
            if (unit == 0x1A) {
                if (end - p < 3 || p[2] < 3 || p[2] > end - p) {
                    return {};
                }
                p += p[2];
                continue;
            }
            uint32_t cp = unit;
            if (unit >= 0xD800 && unit < 0xDC00 && end - p >= 4) {
                const uint16_t low = be16(p + 2);
                cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                p += 2;
            }
            appendCodePoint(out, cp);
            p += 2;
        }
    } else {
        while (p < end && *p != 0) {
            if (*p == 0x1A) {
                if (end - p < 2 || p[1] < 2 || p[1] > end - p) {
                    return {};
                }
                p += p[1];
                continue;
            }
            if (encoding == 4) {  // UTF-8
                const uint8_t lead = *p;
                const int len = lead < 0x80    ? 1
                                : lead >= 0xF0 ? 4
                                : lead >= 0xE0 ? 3
                                : lead >= 0xC0 ? 2
                                               : 0;
                if (len == 0 || len > end - p) {
                    return {};
                }
                if (len == 1) {
                    appendCodePoint(out, lead);
                } else if (out.size() + len <= kMaxNameBytes) {
                    out.append(reinterpret_cast<const char*>(p), len);
                }
                p += len;
                continue;
            }
            const uint8_t c = *p++;  // Windows-1252 (US and PAL discs)
            appendCodePoint(out, c >= 0x80 && c < 0xA0 ? kCp1252High[c - 0x80] : c);
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

// Message `messageId` of zel_00.bmg (message archive 0), every offset checked.
std::string gameMessage(uint32_t messageId) {
    JKRArchive* arc = dComIfGp_getMsgDtArchive(0);
    if (arc == nullptr) {
        return {};
    }
    const auto* bmg = static_cast<const uint8_t*>(JKRGetTypeResource('ROOT', "zel_00.bmg", arc));
    if (bmg == nullptr || std::memcmp(bmg, "MESGbmg1", 8) != 0) {
        return {};
    }
    const uint32_t fileSize = be32(bmg + 0x08);
    const uint32_t sections = be32(bmg + 0x0C);
    const uint8_t encoding = bmg[0x10];
    const uint8_t* inf = nullptr;
    uint32_t infSize = 0;
    const uint8_t* dat = nullptr;
    uint32_t datSize = 0;
    uint32_t offset = 0x20;
    for (uint32_t i = 0; i < sections && offset + 8 <= fileSize; i++) {
        const uint32_t magic = be32(bmg + offset);
        const uint32_t size = be32(bmg + offset + 4);
        if (size < 8 || size > fileSize - offset) {
            break;
        }
        if (magic == 0x494E4631) {  // INF1
            inf = bmg + offset;
            infSize = size;
        } else if (magic == 0x44415431) {  // DAT1
            dat = bmg + offset;
            datSize = size;
        }
        offset += size;
    }
    if (inf == nullptr || dat == nullptr || infSize < 16) {
        return {};
    }
    const uint16_t count = be16(inf + 8);
    const uint16_t entrySize = be16(inf + 10);
    if (entrySize < 6 || 16 + static_cast<size_t>(count) * entrySize > infSize) {
        return {};
    }
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t* entry = inf + 16 + static_cast<size_t>(i) * entrySize;
        if (be16(entry + 4) != messageId) {
            continue;
        }
        const uint32_t stringOffset = be32(entry);
        if (stringOffset >= datSize - 8) {
            return {};
        }
        return decodeMessage(dat + 8 + stringOffset, dat + datSize, encoding);
    }
    return {};
}

bool isUsableName(const std::string& s) {
    for (const char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80) {
            return true;
        }
    }
    return false;
}

std::string fallbackName(uint8_t itemNo) {
    for (const auto& [no, name] : kEnglishNames) {
        if (no == itemNo) {
            return name;
        }
    }
    return fmt::format("item 0x{:02X}", itemNo);
}

const NameEntry& lookup(uint8_t itemNo, int saveTbl) {
    const bool snowpeakMap = itemNo == dItemNo_MAP_e && saveTbl == dStage_SaveTbl_LV5;
    NameEntry& e = snowpeakMap ? sSnowpeakMap : sNames[itemNo];
    if (e.known) {
        return e;
    }
    // Nothing is cached before the message archive is mounted (title screen).
    if (dComIfGp_getMsgDtArchive(0) == nullptr) {
        static NameEntry sTransient;
        sTransient = {false, false, fallbackName(itemNo)};
        return sTransient;
    }
    // The game's heart piece name is the plural, which reads wrong for one pickup.
    std::string text = itemNo == dItemNo_KAKERA_HEART_e
                           ? std::string{}
                           : gameMessage(snowpeakMap ? kSnowpeakMapMessage
                                                     : itemNo + kItemNameMessageBase);
    e.known = true;
    e.fromGame = isUsableName(text);
    e.text = e.fromGame ? std::move(text) : fallbackName(itemNo);
    return e;
}

}  // namespace

const std::string& itemDisplayName(uint8_t itemNo, int saveTbl) {
    return lookup(itemNo, saveTbl).text;
}

bool itemNameFromGame(uint8_t itemNo, int saveTbl) {
    return lookup(itemNo, saveTbl).fromGame;
}

const char* dungeonNameForSaveTbl(int saveTbl) {
    switch (saveTbl) {
    case dStage_SaveTbl_LV1: return "Forest Temple";
    case dStage_SaveTbl_LV2: return "Goron Mines";
    case dStage_SaveTbl_LV3: return "Lakebed Temple";
    case dStage_SaveTbl_LV4: return "Arbiter's Grounds";
    case dStage_SaveTbl_LV5: return "Snowpeak Ruins";
    case dStage_SaveTbl_LV6: return "Temple of Time";
    case dStage_SaveTbl_LV7: return "City in the Sky";
    case dStage_SaveTbl_LV8: return "Palace of Twilight";
    case dStage_SaveTbl_LV9: return "Hyrule Castle";
    default: return "";
    }
}

}  // namespace twili::ui
