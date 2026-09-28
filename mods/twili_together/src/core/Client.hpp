#pragma once

#include "presence/RemotePose.hpp"

#include <cstdint>
#include <string>

namespace twili {

// Where a player's Epona waits while its owner is in another stage.
struct HorsePlace {
    bool valid = false;
    char stage[8] = {};
    int8_t room = -1;
    float pos[3] = {};
    int16_t angleY = 0;
};

struct Client {
    uint32_t clientId = 0;
    std::string name;
    uint8_t colorR = 255, colorG = 255, colorB = 255;
    std::string teamId;
    std::string horseName;
    HorsePlace horsePlace;
    std::string modVersion;
    std::string layout;
    int protocolVersion = 0;
    bool online = false;
    bool self = false;
    bool isSaveLoaded = false;
    bool hasPlayerUpdate = false;

    char stageName[8] = {};
    int8_t layerNo = 0;
    int8_t roomNo = 0;
    int8_t saveTblNo = -1;

    // Latest PLAYER_UPDATE; dummies play the buffered poses in `pose` instead.
    float posX = 0, posY = 0, posZ = 0;
    int16_t angleX = 0, angleY = 0, angleZ = 0;
    int16_t shapeAngleX = 0, shapeAngleY = 0, shapeAngleZ = 0;
    int16_t bodyAngleX = 0, bodyAngleY = 0, bodyAngleZ = 0;
    int16_t bodyTwistY = 0;
    int8_t transformStatus = 0;  // 0 human, 1 wolf
    bool modelSwap = false;
    uint16_t upperANMs[3] = {};
    uint16_t lowerANMs[3] = {};
    float upperFrames[3] = {};
    float lowerFrames[3] = {};
    float upperRatios[3] = {};
    float lowerRatios[3] = {};
    int16_t waterDropColors[2][4] = {};
    int16_t swordUpColors[2][4] = {};
    uint8_t clothesItem = 0xFF;
    uint8_t swordItem = 0xFF;
    uint8_t shieldItem = 0xFF;
    uint16_t equipItem = 0xFFFF;
    uint8_t upperBlendMode = 0;
    float upperBlendRatio = 0.0f;
    uint8_t cutType = 0;
    uint8_t swordBlurAlpha = 0;
    bool swordBlurActive = false;
    uint8_t leftHandIndex = 0xFE;
    uint8_t rightHandIndex = 0xFE;
    uint8_t leftHandItemOverride = 0xFF;
    uint8_t rightHandItemOverride = 0xFF;
    uint8_t leftHandGripOverride = 0xFF;
    uint8_t rightHandGripOverride = 0xFF;
    uint16_t leftItemJoint = 10;
    uint16_t rightItemJoint = 15;
    uint16_t itemBckId = 0xFFFF;
    float itemBckFrame = 0.0f;
    bool itemAmmoLoaded = false;
    uint16_t itemProjectileSeq = 0;
    uint8_t itemProjectileType = 0;
    bool swordChargeActive = false;
    float swordChargeFrame = 0.0f;
    bool attentionLock = false;
    bool shieldInHand = false;
    uint8_t presenceFlags = 0;
    uint16_t visFlags = 0;
    RemoteMidnaPose midna;
    RemoteTransformFx transformFx;
    bool sendsTransformFx = false;
    TransformFxTrace transformTrace;
    RemoteStatusFx status;
    RemoteWolfFx wolfFx;
    RemoteItemFx itemFx;
    bool sendsItemFx = false;
    ItemFxEventQueue itemFxEvents;
    RemoteHorsePose horse;

    // Deltas apply on top of `wire`.
    WirePose wire;
    RemotePoseBuffer pose;
};

}  // namespace twili
