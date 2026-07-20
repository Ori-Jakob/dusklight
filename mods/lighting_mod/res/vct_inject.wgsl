@group(0) @binding(0) var<uniform> frame: FrameUniforms;
@group(0) @binding(1) var scene_color: texture_2d<f32>;
@group(0) @binding(2) var scene_depth: texture_2d<f32>;
@group(0) @binding(3) var normals_tex: texture_2d<f32>;
@group(0) @binding(4) var light_hdr: texture_2d<f32>;

struct VoxelAccum {
    values: array<atomic<u32>>,
}
@group(0) @binding(5) var<storage, read_write> accum: VoxelAccum;

const VCT_FIXED_SCALE: f32 = 64.0;

fn texel_at_uv(size: vec2u, uv: vec2f) -> vec2i {
    return vec2i(min(vec2u(uv * vec2f(size)), size - vec2u(1)));
}

fn inject(cascade: u32, world: vec3f, radiance: vec3f) {
    let cell = vct_world_cell(frame, cascade, world);
    if (!vct_in_window(frame, cascade, cell)) { return; }
    let flat = vct_flat(cascade, vct_texel(cell)) * 4u;
    let encoded = vec3u(clamp(radiance, vec3f(0.0), vec3f(32.0)) * VCT_FIXED_SCALE);
    atomicAdd(&accum.values[flat], encoded.r);
    atomicAdd(&accum.values[flat + 1u], encoded.g);
    atomicAdd(&accum.values[flat + 2u], encoded.b);
    atomicAdd(&accum.values[flat + 3u], 1u);
}

@compute @workgroup_size(8, 8, 1)
fn inject_voxels(@builtin(global_invocation_id) invocation: vec3u) {
    let half_size = vec2u(max(frame.half_size, vec2f(1.0)));
    if (any(invocation.xy >= half_size)) { return; }
    let full_size = textureDimensions(scene_depth);
    let full_px = min(vec2u((vec2f(invocation.xy) + 0.5) * vec2f(full_size) /
                            vec2f(half_size)), full_size - vec2u(1));
    let depth = textureLoad(scene_depth, vec2i(full_px), 0).x;
    if (is_sky(depth)) { return; }
    let uv = (vec2f(full_px) + 0.5) / vec2f(full_size);
    let world = unproject(frame.world_from_proj, ndc_from_uv(uv, depth));
    let scene = textureLoad(scene_color, vec2i(full_px), 0).rgb;
    let direct_size = textureDimensions(light_hdr);
    let direct = textureLoad(light_hdr, texel_at_uv(direct_size, uv), 0).rgb;
    let radiance = bounce_source(frame, scene, direct);
    inject(0u, world, radiance);
    inject(1u, world, radiance);
    inject(2u, world, radiance);
}
