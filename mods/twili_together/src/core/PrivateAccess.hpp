#pragma once

// J3DTexture keeps its GX objects private; explicit instantiations may name private members.

#include "JSystem/J3DGraphBase/J3DTexture.h"

namespace twili::detail {

template <typename Tag, typename Tag::type Member>
struct PrivateMember {
    friend typename Tag::type privateMember(Tag) { return Member; }
};

struct J3DTextureTexObj {
    using type = TGXTexObj* J3DTexture::*;
    friend type privateMember(J3DTextureTexObj);
};
struct J3DTextureTlutObj {
    using type = GXTlutObj* J3DTexture::*;
    friend type privateMember(J3DTextureTlutObj);
};
struct J3DTextureTlutData {
    using type = u8** J3DTexture::*;
    friend type privateMember(J3DTextureTlutData);
};

template struct PrivateMember<J3DTextureTexObj, &J3DTexture::mpTexObj>;
template struct PrivateMember<J3DTextureTlutObj, &J3DTexture::mpTlutObj>;
template struct PrivateMember<J3DTextureTlutData, &J3DTexture::mpTlutDataPtr>;

}  // namespace twili::detail

namespace twili {

inline u8* textureTlutData(const J3DTexture& tex, u16 index) {
    return (tex.*privateMember(detail::J3DTextureTlutData{}))[index];
}

// The image (or palette) bytes of `index` were rewritten in place: aurora decodes them again.
inline void notifyTextureDataChanged(J3DTexture& tex, u16 index) {
    GXInitTexObjData(&(tex.*privateMember(detail::J3DTextureTexObj{}))[index],
        tex.getImgDataPtr(index));
    if (tex.getResTIMG(index)->indexTexture) {
        GXInitTlutObjData(
            &(tex.*privateMember(detail::J3DTextureTlutObj{}))[index], textureTlutData(tex, index));
    }
}

}  // namespace twili
