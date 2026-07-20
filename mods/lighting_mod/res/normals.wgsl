@group(0) @binding(0) var<uniform> frame: FrameUniforms;
@group(0) @binding(1) var scene_depth: texture_2d<f32>;
@group(0) @binding(2) var normals_out: texture_storage_2d<rgba16float, write>;

fn depth_position(px: vec2i, size: vec2u) -> vec3f {
    let clamped = clamp(px, vec2i(0), vec2i(size) - vec2i(1));
    let uv = (vec2f(clamped) + 0.5) / vec2f(size);
    let depth = textureLoad(scene_depth, clamped, 0).x;
    return unproject(frame.world_from_proj, ndc_from_uv(uv, depth));
}

@compute @workgroup_size(8, 8, 1)
fn reconstruct_normals(@builtin(global_invocation_id) invocation: vec3u) {
    let size = textureDimensions(normals_out);
    if (any(invocation.xy >= size)) {
        return;
    }
    let p = vec2i(invocation.xy);
    let depth = textureLoad(scene_depth, p, 0).x;
    if (is_sky(depth)) {
        textureStore(normals_out, p, vec4f(0.0));
        return;
    }

    let center = depth_position(p, size);
    let left1 = depth_position(p + vec2i(-1, 0), size);
    let left2 = depth_position(p + vec2i(-2, 0), size);
    let right1 = depth_position(p + vec2i(1, 0), size);
    let right2 = depth_position(p + vec2i(2, 0), size);
    let up1 = depth_position(p + vec2i(0, -1), size);
    let up2 = depth_position(p + vec2i(0, -2), size);
    let down1 = depth_position(p + vec2i(0, 1), size);
    let down2 = depth_position(p + vec2i(0, 2), size);

    // Pick the one-sided derivative whose three samples have the smaller second derivative.
    // This avoids crossing depth discontinuities while retaining accurate planar normals.
    let left_error = length(left2 - 2.0 * left1 + center);
    let right_error = length(center - 2.0 * right1 + right2);
    let up_error = length(up2 - 2.0 * up1 + center);
    let down_error = length(center - 2.0 * down1 + down2);
    let tangent_x = select(center - left1, right1 - center, right_error < left_error);
    let tangent_y = select(center - up1, down1 - center, down_error < up_error);
    var normal = normalize(cross(tangent_y, tangent_x));
    let toward_eye = normalize(frame.eye - center);
    if (dot(normal, toward_eye) < 0.0) {
        normal = -normal;
    }
    textureStore(normals_out, p, vec4f(normal, 1.0));
}
