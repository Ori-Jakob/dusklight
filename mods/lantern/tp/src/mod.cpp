#include "nameplates.hpp"
#include "presence.hpp"

#include "lantern/service.h"
#include "protocol.hpp"

#include <mods/hook.hpp>
#include <mods/service.hpp>
#include <mods/svc/gfx.h>
#include <mods/svc/hook.h>
#include <mods/svc/log.h>
#include <mods/svc/resource.h>

#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DTexture.h"
#include "JSystem/J3DGraphBase/J3DVertex.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_resorce.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"

namespace bmdl_res {
#include "res/Object/Bmdl.h"
}
namespace kmdl_res {
#include "res/Object/Kmdl.h"
}
namespace mmdl_res {
#include "res/Object/Mmdl.h"
}
namespace wmdl_res {
#include "res/Object/Wmdl.h"
}
namespace zmdl_res {
#include "res/Object/Zmdl.h"
}

#include <dolphin/mtx.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

DEFINE_MOD();
IMPORT_SERVICE(LanternService, svc_lantern);
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(ResourceService, svc_resource);

DEFINE_HOOK_SYMBOL("daAlink_Draw", int(daAlink_c*), LinkDraw);

namespace {

constexpr size_t kMaxBufferedPoses = 32;
constexpr u32 kLinkModelFlags = J3DMdlFlag_DifferedDLBuffer;
constexpr u32 kLinkDifferedDisplayListFlags = 0x11000084;
constexpr u32 kWarpMaterialDifferedDisplayListFlags = 0x2000400;

J3DModel* create_link_model(J3DModelData* model_data) {
    if (model_data == nullptr)
        return nullptr;

    bool warp_material = false;
    J3DTexture* textures = model_data->getTexture();
    if (textures != nullptr && textures->getNum() != 0) {
        auto* warp_texture =
            static_cast<ResTIMG*>(dComIfG_getObjectRes("Always", dRes_ID_ALWAYS_BTI_WARP_TEX_e));
        if (warp_texture != nullptr) {
            const auto* warp_image = reinterpret_cast<const u8*>(warp_texture) +
                                     static_cast<uintptr_t>(warp_texture->imageOffset);
            warp_material = textures->getImgDataPtr(textures->getNum() - 1) == warp_image;
        }
    }

    u32 differed_flags = kLinkDifferedDisplayListFlags;
    if (warp_material) {
        dRes_info_c::onWarpMaterial(model_data);
        differed_flags |= kWarpMaterialDifferedDisplayListFlags;
    }
    J3DModel* model = mDoExt_J3DModel__create(model_data, kLinkModelFlags, differed_flags);
    if (warp_material)
        dRes_info_c::offWarpMaterial(model_data);
    return model;
}

void calc_clone_model(J3DModel* model) {
    struct SavedCallback {
        J3DJoint* joint;
        J3DJointCallBack callback;
    };

    J3DModelData* model_data = model->getModelData();
    std::vector<SavedCallback> callbacks;
    callbacks.reserve(model_data->getJointNum());
    // Link installs actor callbacks directly on the shared model data. A clone has no daAlink_c
    // user area, so invoking those callbacks dereferences null. Suppress them only for this
    // game-thread calculation, then restore the shared resource before returning to Link.
    for (u16 index = 0; index < model_data->getJointNum(); ++index) {
        J3DJoint* joint = model_data->getJointNodePointer(index);
        callbacks.push_back({joint, joint->getCallBack()});
        joint->setCallBack(nullptr);
    }
    model->calc();
    for (const SavedCallback& saved : callbacks)
        saved.joint->setCallBack(saved.callback);
}

class Presence {
public:
    ModResult initialize(ModError* error) {
        LanternModuleDesc module = LANTERN_MODULE_DESC_INIT;
        module.module_id = "dev.twilitrealm.lantern.tp.presence";
        module.display_version = "0.1.5";
        module.protocol_major = 2;
        module.protocol_minor = 0;
        module.minimum_peer_minor = 0;
        module.flags = LANTERN_MODULE_REQUIRED;
        module.distribution_id = "dev.twilitrealm.lantern.tp";
        module.on_session = &Presence::session_callback;
        module.on_peer = &Presence::peer_callback;
        module.on_message = &Presence::message_callback;
        module.user_data = this;
        const ModResult registration =
            svc_lantern->register_module(mod_ctx, &module, &module_handle_);
        if (registration != MOD_OK) {
            return mods::set_error(error, registration, "Could not register Lantern TP presence");
        }
        if (!lantern::tp::nameplates::initialize(mod_ctx, svc_gfx, svc_resource)) {
            svc_lantern->unregister_module(mod_ctx, module_handle_);
            module_handle_ = 0;
            return mods::set_error(error, MOD_ERROR, "Could not initialize Lantern nameplates");
        }
        HookOptions options = HOOK_OPTIONS_INIT;
        options.priority = -100;  // Capture after Link and render after other ordinary post hooks.
        const ModResult hook =
            mods::hook_add_post<LinkDraw>(svc_hook, &Presence::link_draw_post, &options);
        if (hook != MOD_OK) {
            lantern::tp::nameplates::shutdown();
            svc_lantern->unregister_module(mod_ctx, module_handle_);
            module_handle_ = 0;
            return mods::set_error(error, hook, "Could not hook Link's draw function");
        }
        svc_log->info(mod_ctx, "Lantern TP presence initialized");
        return MOD_OK;
    }

    ModResult update(ModError*) {
        const auto now = std::chrono::steady_clock::now();
        // mod_update runs once per 30 Hz game simulation tick. Sending once here keeps presence
        // aligned to the authoritative animation cadence; a wall-clock interval is quantized by
        // those ticks and the old 50 ms gate therefore produced an effective 15 Hz stream.
        if (local_valid_ && svc_lantern->connection_state(mod_ctx) == LANTERN_CONNECTED) {
            local_pose_.sequence = next_pose_sequence_++;
            local_pose_.timestamp_ms = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                    .count());
            std::vector<uint8_t> encoded;
            std::string error;
            if (lantern::tp::encode_pose(local_pose_, encoded, error)) {
                LanternSendDesc send = LANTERN_SEND_DESC_INIT;
                send.module = module_handle_;
                send.delivery = LANTERN_UNRELIABLE_LATEST;
                send.data = encoded.data();
                send.size = encoded.size();
                svc_lantern->send(mod_ctx, &send);
            }
        }
        for (auto& [id, remote] : remotes_) {
            (void)id;
            while (remote.poses.size() > 1 &&
                   now - remote.poses.front().received_at > std::chrono::seconds(2))
            {
                remote.poses.pop_front();
            }
        }
        return MOD_OK;
    }

    ModResult shutdown(ModError*) {
        if (module_handle_ != 0) {
            svc_lantern->unregister_module(mod_ctx, module_handle_);
            module_handle_ = 0;
        }
        lantern::tp::nameplates::shutdown();
        clear_remotes();
        svc_log->info(mod_ctx, "Lantern TP presence unloaded");
        return MOD_OK;
    }

private:
    struct Remote {
        std::string display_name;
        uint32_t color = 0xFFFFFF;
        std::deque<lantern::tp::TimedPose> poses;
        J3DModel* human_model = nullptr;
        J3DModel* wolf_model = nullptr;
        J3DModel* human_face_model = nullptr;
        J3DModel* human_head_model = nullptr;
        J3DModel* human_hands_model = nullptr;
        J3DModelData* human_data = nullptr;
        J3DModelData* wolf_data = nullptr;
    };

    static void session_callback(
        ModContext*, LanternSessionEvent event, const char*, void* user_data) {
        if (event == LANTERN_SESSION_DISCONNECTED || event == LANTERN_SESSION_REJECTED) {
            static_cast<Presence*>(user_data)->clear_remotes();
        }
    }

    static void peer_callback(
        ModContext*, LanternPeerEvent event, const LanternPeerInfo* peer, void* user_data) {
        if (peer == nullptr)
            return;
        auto& self = *static_cast<Presence*>(user_data);
        if (event == LANTERN_PEER_LEFT) {
            self.erase_remote(peer->peer_id);
            return;
        }
        auto& remote = self.remotes_[peer->peer_id];
        remote.display_name = peer->display_name == nullptr ? "Player" : peer->display_name;
        remote.color = peer->color_rgb;
    }

    static void message_callback(
        ModContext*, LanternModuleHandle, const LanternMessage* message, void* user_data) {
        if (message == nullptr || message->data == nullptr)
            return;
        auto& self = *static_cast<Presence*>(user_data);
        lantern::tp::PoseSnapshot pose;
        std::string error;
        if (!lantern::tp::decode_pose(
                std::span(static_cast<const uint8_t*>(message->data), message->size), pose, error))
        {
            svc_log->warn(mod_ctx, ("Dropped malformed Lantern presence: " + error).c_str());
            return;
        }
        auto& remote = self.remotes_[message->sender_peer_id];
        if (!remote.poses.empty() &&
            !lantern::protocol::sequence_newer(pose.sequence, remote.poses.back().pose.sequence))
        {
            return;
        }
        if (!remote.poses.empty() && lantern::tp::is_teleport(remote.poses.back().pose, pose)) {
            remote.poses.clear();
        }
        remote.poses.push_back({std::move(pose), std::chrono::steady_clock::now()});
        while (remote.poses.size() > kMaxBufferedPoses)
            remote.poses.pop_front();
    }

    static void link_draw_post(ModContext*, void* args, void*, void* user_data) {
        (void)user_data;
        daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
        if (link == nullptr)
            return;
        instance().capture(link);
        instance().render_remotes(link);
    }

    static Presence& instance() { return g_presence; }

    void capture(daAlink_c* link) {
        lantern::tp::PoseSnapshot snapshot;
        const char* stage = dComIfGp_getStartStageName();
        if (stage == nullptr || stage[0] == '\0') {
            local_valid_ = false;
            return;
        }
        snapshot.stage = stage;
        snapshot.layer = dComIfGp_getStartStageLayer();
        snapshot.room = static_cast<int8_t>(dComIfGp_roomControl_getStayNo());
        snapshot.wolf = link->checkWolf() != 0;
        if (link->mArcName != nullptr) {
            if (std::strcmp(link->mArcName, "Bmdl") == 0) {
                snapshot.human_model = lantern::tp::HumanModelVariant::Ordon;
            } else if (std::strcmp(link->mArcName, "Zmdl") == 0) {
                snapshot.human_model = lantern::tp::HumanModelVariant::Zora;
            } else if (std::strcmp(link->mArcName, "Mmdl") == 0) {
                snapshot.human_model = lantern::tp::HumanModelVariant::Magic;
            }
        }
        snapshot.position = {link->current.pos.x, link->current.pos.y, link->current.pos.z};
        snapshot.rotation = {link->shape_angle.x, link->shape_angle.y, link->shape_angle.z};
        snapshot.velocity = {link->speed.x, link->speed.y, link->speed.z};
        snapshot.equipment = (link->checkItemSwordEquip() ? 1u : 0u) |
                             (link->checkNoEquipItem() ? 0u : 2u) | (snapshot.wolf ? 4u : 0u);

        const size_t joint_count = snapshot.wolf ? 40 : 35;
        MtxP root = link->getModelJointMtx(0);
        if (root == nullptr) {
            local_valid_ = false;
            return;
        }
        Mtx inverse_root{};
        if (!MTXInverse(root, inverse_root)) {
            local_valid_ = false;
            return;
        }
        snapshot.joints.reserve(joint_count);
        for (size_t joint_index = 0; joint_index < joint_count; ++joint_index) {
            MtxP world = link->getModelJointMtx(static_cast<u16>(joint_index));
            if (world == nullptr)
                break;
            Mtx relative{};
            if (joint_index == 0) {
                MTXCopy(world, relative);
                relative[0][3] -= snapshot.position[0];
                relative[1][3] -= snapshot.position[1];
                relative[2][3] -= snapshot.position[2];
            } else {
                MTXConcat(inverse_root, world, relative);
            }
            lantern::tp::JointTransform transform;
            std::memcpy(transform.matrix.data(), relative, sizeof(relative));
            snapshot.joints.push_back(transform);
        }
        if (snapshot.joints.empty()) {
            local_valid_ = false;
            return;
        }
        local_pose_ = std::move(snapshot);
        local_valid_ = true;
    }

    void render_remotes(daAlink_c* link) {
        if (!local_valid_ || dComIfGp_event_runCheck())
            return;
        const auto now = std::chrono::steady_clock::now();
        for (auto& [peer_id, remote] : remotes_) {
            (void)peer_id;
            lantern::tp::PoseSnapshot pose;
            if (!lantern::tp::interpolate_pose(remote.poses, now, pose) ||
                !lantern::tp::same_area(local_pose_, pose))
            {
                continue;
            }
            J3DModel* model = ensure_model(remote, pose);
            if (model == nullptr || pose.joints.size() != model->getModelData()->getJointNum())
                continue;
            Mtx root{};
            std::memcpy(root, pose.joints[0].matrix.data(), sizeof(root));
            root[0][3] += pose.position[0];
            root[1][3] += pose.position[1];
            root[2][3] += pose.position[2];
            model->setBaseTRMtx(root);
            model->getVertexBuffer()->frameInit();
            for (size_t joint = 0; joint < pose.joints.size(); ++joint) {
                Mtx world{};
                if (joint == 0) {
                    MTXCopy(root, world);
                } else {
                    Mtx relative{};
                    std::memcpy(relative, pose.joints[joint].matrix.data(), sizeof(relative));
                    MTXConcat(root, relative, world);
                }
                model->setAnmMtx(static_cast<int>(joint), world);
                model->setScaleFlag(static_cast<int>(joint), 0);
            }
            // J3D keeps separate weighted-envelope matrices for skinned vertices; those must be
            // rebuilt after replacing the joint matrices or vertices use stale allocations.
            model->calcWeightEnvelopeMtx();
            g_env_light.setLightTevColorType_MAJI(model, &link->tevStr);
            mDoExt_modelEntryDL(model);
            if (!pose.wolf) {
                render_human_attachments(remote, model, root, link);
            }

            Vec label_world{pose.position[0], pose.position[1] + (pose.wolf ? 135.0f : 205.0f),
                pose.position[2]};
            Vec camera_space{};
            mDoLib_pos2camera(&label_world, &camera_space);
            if (camera_space.z < -1.0f) {
                Vec projected{};
                mDoLib_project(&label_world, &projected);
                const float width = mDoGph_gInf_c::getWidthF();
                const float height = mDoGph_gInf_c::getHeightF();
                if (width > 0.0f && height > 0.0f) {
                    const float normalized_x = (projected.x - mDoGph_gInf_c::getMinXF()) / width;
                    const float normalized_y =
                        (projected.y - mDoGph_gInf_c::getMinYF()) / height - 12.0f / 448.0f;
                    if (normalized_x > -0.1f && normalized_x < 1.1f && normalized_y > -0.1f &&
                        normalized_y < 1.1f)
                    {
                        lantern::tp::nameplates::submit(
                            remote.display_name, remote.color, normalized_x, normalized_y);
                    }
                }
            }
        }
    }

    J3DModel* ensure_model(Remote& remote, const lantern::tp::PoseSnapshot& pose) {
        struct ModelResource {
            const char* archive;
            int body;
            int face;
            int head;
            int hands;
        };
        const auto resource_for = [](lantern::tp::HumanModelVariant variant) {
            switch (variant) {
            case lantern::tp::HumanModelVariant::Ordon:
                return ModelResource{"Bmdl", bmdl_res::dRes_ID_BMDL_BMD_BL_e,
                    bmdl_res::dRes_ID_BMDL_BMD_AL_FACE_e, bmdl_res::dRes_ID_BMDL_BMD_BL_HEAD_e,
                    bmdl_res::dRes_ID_BMDL_BMD_BL_HANDS_e};
            case lantern::tp::HumanModelVariant::Zora:
                return ModelResource{"Zmdl", zmdl_res::dRes_ID_ZMDL_BMD_ZL_e,
                    zmdl_res::dRes_ID_ZMDL_BMD_ZL_FACE_e, zmdl_res::dRes_ID_ZMDL_BMD_ZL_HEAD_e,
                    zmdl_res::dRes_ID_ZMDL_BMD_AL_HANDS_e};
            case lantern::tp::HumanModelVariant::Magic:
                return ModelResource{"Mmdl", mmdl_res::dRes_ID_MMDL_BMD_ML_e,
                    mmdl_res::dRes_ID_MMDL_BMD_AL_FACE_e, mmdl_res::dRes_ID_MMDL_BMD_ML_HEAD_e,
                    mmdl_res::dRes_ID_MMDL_BMD_AL_HANDS_e};
            case lantern::tp::HumanModelVariant::Hero:
            default:
                return ModelResource{"Kmdl", kmdl_res::dRes_ID_KMDL_BMD_AL_e,
                    kmdl_res::dRes_ID_KMDL_BMD_AL_FACE_e, kmdl_res::dRes_ID_KMDL_BMD_AL_HEAD_e,
                    kmdl_res::dRes_ID_KMDL_BMD_AL_HANDS_e};
            }
        };
        ModelResource selected =
            pose.wolf ? ModelResource{"Wmdl", wmdl_res::dRes_ID_WMDL_BMD_WL_e, -1, -1, -1} :
                        resource_for(pose.human_model);
        auto* data =
            static_cast<J3DModelData*>(dComIfG_getObjectRes(selected.archive, selected.body));
        // An outfit archive may not be resident on a client using another outfit. Preserve visible
        // presence with that client's resident human body; all four body rigs share 35 joints.
        if (data == nullptr && !pose.wolf) {
            selected = resource_for(local_pose_.human_model);
            data =
                static_cast<J3DModelData*>(dComIfG_getObjectRes(selected.archive, selected.body));
        }
        if (data == nullptr)
            return nullptr;
        J3DModel*& model = pose.wolf ? remote.wolf_model : remote.human_model;
        J3DModelData*& previous_data = pose.wolf ? remote.wolf_data : remote.human_data;
        if (model != nullptr && previous_data != data) {
            delete model;
            model = nullptr;
            if (!pose.wolf) {
                delete remote.human_face_model;
                delete remote.human_head_model;
                delete remote.human_hands_model;
                remote.human_face_model = nullptr;
                remote.human_head_model = nullptr;
                remote.human_hands_model = nullptr;
            }
        }
        if (model == nullptr) {
            model = create_link_model(data);
            previous_data = data;
            if (model != nullptr && !pose.wolf) {
                const auto create_part = [&](int resource) {
                    auto* part_data = static_cast<J3DModelData*>(
                        dComIfG_getObjectRes(selected.archive, resource));
                    if (part_data == nullptr) {
                        return static_cast<J3DModel*>(nullptr);
                    }
                    return create_link_model(part_data);
                };
                remote.human_face_model = create_part(selected.face);
                remote.human_head_model = create_part(selected.head);
                remote.human_hands_model = create_part(selected.hands);
            }
        }
        return model;
    }

    static void render_human_attachments(
        Remote& remote, J3DModel* body, Mtx root, daAlink_c* link) {
        if (remote.human_face_model != nullptr) {
            remote.human_face_model->setBaseTRMtx(body->getAnmMtx(4));
            calc_clone_model(remote.human_face_model);
            g_env_light.setLightTevColorType_MAJI(remote.human_face_model, &link->tevStr);
            mDoExt_modelEntryDL(remote.human_face_model);
        }
        if (remote.human_head_model != nullptr) {
            remote.human_head_model->setBaseTRMtx(body->getAnmMtx(4));
            calc_clone_model(remote.human_head_model);
            g_env_light.setLightTevColorType_MAJI(remote.human_head_model, &link->tevStr);
            mDoExt_modelEntryDL(remote.human_head_model);
        }
        if (remote.human_hands_model != nullptr) {
            remote.human_hands_model->setBaseTRMtx(root);
            calc_clone_model(remote.human_hands_model);
            if (remote.human_hands_model->getModelData()->getJointNum() >= 3) {
                remote.human_hands_model->setAnmMtx(1, body->getAnmMtx(9));
                remote.human_hands_model->setAnmMtx(2, body->getAnmMtx(14));
                remote.human_hands_model->calcWeightEnvelopeMtx();
            }
            g_env_light.setLightTevColorType_MAJI(remote.human_hands_model, &link->tevStr);
            mDoExt_modelEntryDL(remote.human_hands_model);
        }
    }

    void erase_remote(LanternPeerId id) {
        const auto it = remotes_.find(id);
        if (it == remotes_.end())
            return;
        delete it->second.human_model;
        delete it->second.wolf_model;
        delete it->second.human_face_model;
        delete it->second.human_head_model;
        delete it->second.human_hands_model;
        remotes_.erase(it);
    }

    void clear_remotes() {
        for (auto& [id, remote] : remotes_) {
            (void)id;
            delete remote.human_model;
            delete remote.wolf_model;
            delete remote.human_face_model;
            delete remote.human_head_model;
            delete remote.human_hands_model;
        }
        remotes_.clear();
    }

public:
    static Presence g_presence;

private:
    LanternModuleHandle module_handle_ = 0;
    std::map<LanternPeerId, Remote> remotes_;
    lantern::tp::PoseSnapshot local_pose_;
    bool local_valid_ = false;
    uint32_t next_pose_sequence_ = 1;
};

Presence Presence::g_presence;

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    return Presence::g_presence.initialize(error);
}

MOD_EXPORT ModResult mod_update(ModError* error) {
    return Presence::g_presence.update(error);
}

MOD_EXPORT ModResult mod_shutdown(ModError* error) {
    return Presence::g_presence.shutdown(error);
}
}
