@group(0) @binding(0) var<uniform> frame: FrameUniforms;

struct VoxelAccum {
    values: array<atomic<u32>>,
}
@group(0) @binding(1) var<storage, read_write> accum: VoxelAccum;
@group(0) @binding(2) var old_clip0: texture_3d<f32>;
@group(0) @binding(3) var old_clip1: texture_3d<f32>;
@group(0) @binding(4) var old_clip2: texture_3d<f32>;
@group(0) @binding(5) var new_clip0: texture_storage_3d<rgba16float, write>;
@group(0) @binding(6) var new_clip1: texture_storage_3d<rgba16float, write>;
@group(0) @binding(7) var new_clip2: texture_storage_3d<rgba16float, write>;

const VCT_FIXED_SCALE: f32 = 64.0;

fn previous_origin(cascade: u32) -> vec3i {
    if (cascade == 0u) { return vec3i(frame.vct_prev_origin0); }
    if (cascade == 1u) { return vec3i(frame.vct_prev_origin1); }
    return vec3i(frame.vct_prev_origin2);
}

fn was_in_previous_window(cascade: u32, cell: vec3i) -> bool {
    let relative = cell - previous_origin(cascade);
    return all(relative >= vec3i(0)) && all(relative < vec3i(i32(VCT_DIM)));
}

fn load_old(cascade: u32, texel: vec3u) -> vec4f {
    if (cascade == 0u) { return textureLoad(old_clip0, vec3i(texel), 0); }
    if (cascade == 1u) { return textureLoad(old_clip1, vec3i(texel), 0); }
    return textureLoad(old_clip2, vec3i(texel), 0);
}

fn store_new(cascade: u32, texel: vec3u, value: vec4f) {
    if (cascade == 0u) { textureStore(new_clip0, vec3i(texel), value); return; }
    if (cascade == 1u) { textureStore(new_clip1, vec3i(texel), value); return; }
    textureStore(new_clip2, vec3i(texel), value);
}

@compute @workgroup_size(4, 4, 4)
fn resolve_voxels(@builtin(global_invocation_id) invocation: vec3u) {
    let cascade = invocation.z / VCT_DIM;
    let local = vec3u(invocation.x, invocation.y, invocation.z % VCT_DIM);
    if (cascade >= VCT_CASCADES || any(local >= vec3u(VCT_DIM))) { return; }
    let cell = vec3i(vct_origin(frame, cascade)) + vec3i(local);
    let texel = vct_texel(cell);
    var previous = vec4f(0.0);
    if (was_in_previous_window(cascade, cell)) { previous = load_old(cascade, texel); }

    let flat = vct_flat(cascade, texel) * 4u;
    let red = atomicExchange(&accum.values[flat], 0u);
    let green = atomicExchange(&accum.values[flat + 1u], 0u);
    let blue = atomicExchange(&accum.values[flat + 2u], 0u);
    let count = atomicExchange(&accum.values[flat + 3u], 0u);
    var value = previous * frame.vct_decay;
    if (count > 0u) {
        let fresh = vec3f(f32(red), f32(green), f32(blue)) /
                    (f32(count) * VCT_FIXED_SCALE);
        let radiance = mix(previous.rgb * frame.vct_decay, fresh, 0.25);
        let opacity =
            max(previous.a * frame.vct_decay, clamp(f32(count) * 0.25, 0.0, 1.0));
        value = vec4f(radiance, opacity);
    }
    store_new(cascade, texel, value);
}

@group(0) @binding(8) var mip_source: texture_3d<f32>;
@group(0) @binding(9) var mip_out: texture_storage_3d<rgba16float, write>;

@compute @workgroup_size(4, 4, 4)
fn downsample_voxels(@builtin(global_invocation_id) invocation: vec3u) {
    let size = textureDimensions(mip_out);
    if (any(invocation >= size)) { return; }
    let base = vec3i(invocation * 2u);
    var sum = vec4f(0.0);
    for (var z = 0; z < 2; z++) {
        for (var y = 0; y < 2; y++) {
            for (var x = 0; x < 2; x++) {
                sum += textureLoad(mip_source, base + vec3i(x, y, z), 0);
            }
        }
    }
    textureStore(mip_out, vec3i(invocation), sum * 0.125);
}
