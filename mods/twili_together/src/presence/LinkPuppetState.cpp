#include "core/Client.hpp"
#include "presence/RemotePose.hpp"

namespace twili {

LinkPuppetState makeLinkPuppetState(const Client& client) {
    LinkPuppetState state;
    state.posX = client.posX;
    state.posY = client.posY;
    state.posZ = client.posZ;
    state.angleX = client.angleX;
    state.angleY = client.angleY;
    state.angleZ = client.angleZ;
    state.shapeAngleX = client.shapeAngleX;
    state.shapeAngleY = client.shapeAngleY;
    state.shapeAngleZ = client.shapeAngleZ;
    state.colorR = client.colorR;
    state.colorG = client.colorG;
    state.colorB = client.colorB;
    state.bodyAngleX = client.bodyAngleX;
    state.bodyAngleY = client.bodyAngleY;
    state.bodyAngleZ = client.bodyAngleZ;
    state.bodyTwistY = client.bodyTwistY;
    for (int i = 0; i < 3; i++) {
        state.upperANMs[i] = client.upperANMs[i];
        state.lowerANMs[i] = client.lowerANMs[i];
        state.upperFrames[i] = client.upperFrames[i];
        state.lowerFrames[i] = client.lowerFrames[i];
        state.upperRatios[i] = client.upperRatios[i];
        state.lowerRatios[i] = client.lowerRatios[i];
    }
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 4; j++) {
            state.waterDropColors[i][j] = client.waterDropColors[i][j];
            state.swordUpColors[i][j] = client.swordUpColors[i][j];
        }
    }
    state.swordItem = client.swordItem;
    state.shieldItem = client.shieldItem;
    state.clothesItem = client.clothesItem;
    state.equipItem = client.equipItem;
    state.upperBlendMode = client.upperBlendMode;
    state.upperBlendRatio = client.upperBlendRatio;
    state.cutType = client.cutType;
    state.swordBlurAlpha = client.swordBlurAlpha;
    state.swordBlurActive = client.swordBlurActive;
    state.leftHandIndex = client.leftHandIndex;
    state.rightHandIndex = client.rightHandIndex;
    state.leftHandItemOverride = client.leftHandItemOverride;
    state.rightHandItemOverride = client.rightHandItemOverride;
    state.leftHandGripOverride = client.leftHandGripOverride;
    state.rightHandGripOverride = client.rightHandGripOverride;
    state.leftItemJoint = client.leftItemJoint;
    state.rightItemJoint = client.rightItemJoint;
    state.itemBckId = client.itemBckId;
    state.itemBckFrame = client.itemBckFrame;
    state.itemAmmoLoaded = client.itemAmmoLoaded;
    state.itemProjectileSeq = client.itemProjectileSeq;
    state.itemProjectileType = client.itemProjectileType;
    state.swordChargeActive = client.swordChargeActive;
    state.swordChargeFrame = client.swordChargeFrame;
    state.attentionLock = client.attentionLock;
    state.shieldInHand = client.shieldInHand;
    state.transformStatus = client.transformStatus;
    state.modelSwap = client.modelSwap;
    state.visFlags = client.visFlags;
    state.midna = client.midna;
    state.tf = client.transformFx;
    state.status = client.status;
    state.wolfFx = client.wolfFx;
    state.itemFx = client.itemFx;
    state.sendsItemFx = client.sendsItemFx;
    state.horse = client.horse;
    return state;
}

}  // namespace twili

