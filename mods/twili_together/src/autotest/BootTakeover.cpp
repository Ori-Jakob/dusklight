#include "autotest/AutoTest.hpp"
#include "autotest/State.hpp"

#include "core/Log.hpp"
#include "core/SaveGate.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_kankyo.h"
#include "d/d_meter2_info.h"
#include "f_op/f_op_msg_mng.h"
#include "f_op/f_op_scene_mng.h"
#include "f_pc/f_pc_name.h"

namespace twili::autotest {
namespace detail {

// Gameplay part of dComIfG_playerStatusD, which only exists in DEBUG builds.
void applyDebugStatus() {
    dComIfGs_setDataNum(0);
    dComIfGs_setMaxLife(50);
    dComIfGs_setLife(20);
    dComIfGs_setRupee(64);
    dComIfGs_setMaxMagic(32);
    dComIfGs_setMagic(16);
    dComIfGs_setWalletSize(1);
    dComIfGs_setMaxOil(21600);
    dComIfGs_setOil(21600);
    dComIfGp_setMaxOxygen(600);
    dComIfGp_setOxygen(600);

    for (int i = 0; i < 4; i++) {
        dComIfGs_setMixItemIndex(i, 0xFF);
    }
    dComIfGs_setSelectItemIndex(0, SLOT_0);
    dComIfGs_setSelectItemIndex(1, SLOT_4);
    dComIfGs_setSelectItemIndex(2, 0xFF);
    dComIfGs_setSelectItemIndex(3, 0xFF);
    for (int i = 23; i >= 0; i--) {
        dComIfGs_setItem(i, fopMsgM_itemNumIdx(i));
    }
    for (int i = 0; i < 0x100; i++) {
        dComIfGs_onItemFirstBit(i);
    }
    const u8 notOwned[] = {
        dItemNo_L2_KEY_PIECES1_e,
        dItemNo_L2_KEY_PIECES2_e,
        dItemNo_L2_KEY_PIECES3_e,
        dItemNo_LV2_BOSS_KEY_e,
        dItemNo_BOMB_BAG_LV2_e,
        dItemNo_TOMATO_PUREE_e,
        dItemNo_TASTE_e,
        dItemNo_POU_FIRE1_e,
        dItemNo_POU_FIRE2_e,
        dItemNo_POU_FIRE3_e,
        dItemNo_POU_FIRE4_e,
        dItemNo_LIGHT_SWORD_e,
        dItemNo_SHIELD_e,
        dItemNo_ZORAS_JEWEL_e,
        dItemNo_SMELL_POH_e,
    };
    for (u8 item : notOwned) {
        dComIfGs_offItemFirstBit(item);
    }
    for (int i = 0; i < 24; i++) {
        dComIfGs_offItemFirstBit(i + dItemNo_M_BEETLE_e);
    }
    for (int i = 0; i < 19; i++) {
        dComIfGs_offItemFirstBit(i);
    }
    dComIfGs_setCollectSmell(dItemNo_SMELL_PUMPKIN_e);

    dComIfGs_setArrowNum(30);
    dComIfGs_setArrowMax(30);
    dComIfGs_setPachinkoNum(dComIfGs_getPachinkoMax());
    dComIfGs_setBombNum(0, 30);
    dComIfGs_setBombNum(1, 15);
    dComIfGs_setBombNum(2, 10);
    for (int i = 0; i < 4; i++) {
        dComIfGs_setBottleNum(i, dComIfGs_getBottleMax());
    }
    dComIfGs_setBombNum(8, 30);
    dComIfGs_setBombMax(dItemNo_NORMAL_BOMB_e, 30);
    dComIfGs_setBombMax(dItemNo_WATER_BOMB_e, 15);
    dComIfGs_setBombMax(dItemNo_POKE_BOMB_e, 10);

    dMeter2Info_setCloth(dItemNo_WEAR_KOKIRI_e, false);
    dMeter2Info_setSword(dItemNo_SWORD_e, false);
    dMeter2Info_setShield(dItemNo_HYLIA_SHIELD_e, false);
    dComIfGs_onGetMagicUseFlag();
    dComIfGs_onEventBit(0x540);
    dComIfGs_onEventBit(0xc10);
    dComIfGs_onEventBit(0x510);
    dMeter2Info_offTempBit(0);
    dComIfGs_onEventBit(0x5c01);
    dComIfGs_onEventBit(0x5d80);
}

}  // namespace detail

using namespace detail;

bool takeOverBoot(scene_class* logoScene) {
    State& s = state();
    if (!s.active || !s.initFailure.empty()) {
        return false;
    }
    // Keep skipping: the original would point the next stage back at the title.
    if (s.bootTaken) {
        return true;
    }
    s.bootTaken = true;

    const std::string stage = s.start.value("stage", std::string("R_SP01"));
    const int point = s.start.value("point", 0);
    const int room = s.start.value("room", 4);
    const int layer = s.start.value("layer", -1);
    // new, armed (tunic, sword, shield) or debug (full inventory).
    const std::string status = s.start.value("status", std::string("armed"));

    // Same sequence as the debug map select from a fresh save.
    dComIfGs_init();
    // The default names' message archive is not loaded yet.
    dComIfGs_setPlayerName(s.instance.c_str());
    dComIfGs_setHorseName(s.start.value("horseName", std::string("Epona")).c_str());
    if (status == "debug") {
        applyDebugStatus();
    } else if (status == "armed") {
        dMeter2Info_setCloth(dItemNo_WEAR_KOKIRI_e, false);
        dMeter2Info_setSword(dItemNo_SWORD_e, false);
        dMeter2Info_setShield(dItemNo_WOOD_SHIELD_e, false);
    }
    // A first-pickup message would pause the world and stall the script.
    if (!s.start.value("firstPickups", false)) {
        for (const u8 rupee : {dItemNo_GREEN_RUPEE_e, dItemNo_BLUE_RUPEE_e, dItemNo_YELLOW_RUPEE_e,
                 dItemNo_RED_RUPEE_e, dItemNo_PURPLE_RUPEE_e, dItemNo_ORANGE_RUPEE_e,
                 dItemNo_SILVER_RUPEE_e})
        {
            dComIfGs_onItemFirstBit(rupee);
        }
    }
    // Opt-in: it moves many stages to other layers.
    if (s.start.value("clearTwilight", false)) {
        for (int i = 0; i <= 5; i++) {
            dComIfGs_onDarkClearLV(i);
        }
    }
    // Before the first load, so it already picks the story layer.
    if (const auto bits = s.start.find("eventBits"); bits != s.start.end() && bits->is_array()) {
        for (const auto& no : *bits) {
            if (no.is_number_integer()) {
                dComIfGs_onEventBit(static_cast<u16>(no.get<int>()));
            }
        }
    }
    if (const auto levels = s.start.find("levels"); levels != s.start.end() && levels->is_object())
    {
        for (const auto& n : levels->value("transform", nlohmann::json::array())) {
            if (n.is_number_integer()) {
                dComIfGs_onTransformLV(n.get<int>());
            }
        }
        for (const auto& n : levels->value("darkClear", nlohmann::json::array())) {
            if (n.is_number_integer()) {
                dComIfGs_onDarkClearLV(n.get<int>());
            }
        }
    }
    applyStoryStart(s.start);
    if (s.start.contains("hour")) {
        const float hour = s.start.value("hour", 12.0f);
        dComIfGs_setTime(15.0f * hour);
        g_env_light.daytime = 15.0f * hour;
    }

    dComIfGp_offEnableNextStage();
    dComIfGp_setNextStage(
        stage.c_str(), static_cast<s16>(point), static_cast<s8>(room), static_cast<s8>(layer));
    fopScnM_ChangeReq(logoScene, fpcNm_PLAY_SCENE_e, 0, 5);
    dKy_clear_game_init();
    dComIfGs_resetDan();
    dComIfGs_setRestartRoomParam(0);

    // A fresh save that never went through card_to_memory.
    markSaveLoaded();

    TwiliLog.info("[autotest] boot -> {} point {} room {} layer {} (status {})", stage, point, room,
        layer, status);
    return true;
}

}  // namespace twili::autotest
