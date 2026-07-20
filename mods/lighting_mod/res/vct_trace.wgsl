@group(0) @binding(0) var<uniform> frame: FrameUniforms;
@group(0) @binding(1) var scene_depth: texture_2d<f32>;
@group(0) @binding(2) var normals_tex: texture_2d<f32>;
@group(0) @binding(3) var clip0: texture_3d<f32>;
@group(0) @binding(4) var clip1: texture_3d<f32>;
@group(0) @binding(5) var clip2: texture_3d<f32>;
@group(0) @binding(6) var gi_raw_out: texture_storage_2d<rgba16float, write>;

fn sample_clip(cascade: u32, world: vec3f, cone_diameter: f32) -> vec4f {
    let cell = vct_world_cell(frame, cascade, world);
    if (!vct_in_window(frame, cascade, cell)) { return vec4f(0.0); }
    let voxel_size = vct_voxel_size(frame, cascade);
    let lod = clamp(i32(floor(log2(max(cone_diameter / voxel_size, 1.0)))), 0, 6);
    let texel = vct_texel(cell) >> vec3u(u32(lod));
    if (cascade == 0u) { return textureLoad(clip0, vec3i(texel), lod); }
    if (cascade == 1u) { return textureLoad(clip1, vec3i(texel), lod); }
    return textureLoad(clip2, vec3i(texel), lod);
}

fn choose_cascade(world: vec3f) -> u32 {
    if (vct_in_window(frame, 0u, vct_world_cell(frame, 0u, world))) { return 0u; }
    if (vct_in_window(frame, 1u, vct_world_cell(frame, 1u, world))) { return 1u; }
    if (vct_in_window(frame, 2u, vct_world_cell(frame, 2u, world))) { return 2u; }
    return VCT_CASCADES;
}

fn trace_cone(origin: vec3f, direction: vec3f, aperture: f32) -> vec3f {
    var distance = frame.vct_voxel0 * frame.vct_cone_offset;
    var opacity = 0.0;
    var result = vec3f(0.0);
    for (var step = 0u; step < 20u && opacity < 0.98; step++) {
        let sample_world = origin + direction * distance;
        let cascade = choose_cascade(sample_world);
        if (cascade >= VCT_CASCADES) { break; }
        let cone_diameter = max(2.0 * aperture * distance, vct_voxel_size(frame, cascade));
        let sample = sample_clip(cascade, sample_world, cone_diameter);
        let contribution = (1.0 - opacity) * sample.a;
        result += sample.rgb * contribution;
        opacity += contribution;
        distance += max(cone_diameter * 0.75, vct_voxel_size(frame, cascade));
    }
    return result;
}

@compute @workgroup_size(8, 8, 1)
fn trace_vct(@builtin(global_invocation_id) invocation: vec3u) {
    let out_size = textureDimensions(gi_raw_out);
    if (any(invocation.xy >= out_size)) { return; }
    let full_size = textureDimensions(scene_depth);
    let full_px = min(vec2u((vec2f(invocation.xy) + 0.5) * vec2f(full_size) / vec2f(out_size)),
                      full_size - vec2u(1));
    let depth = textureLoad(scene_depth, vec2i(full_px), 0).x;
    if (is_sky(depth)) {
        textureStore(gi_raw_out, vec2i(invocation.xy), vec4f(0.0));
        return;
    }
    let uv = (vec2f(full_px) + 0.5) / vec2f(full_size);
    let world = unproject(frame.world_from_proj, ndc_from_uv(uv, depth));
    let normal = normalize(textureLoad(normals_tex, vec2i(full_px), 0).xyz);
    let basis = basis_from_normal(normal);
    let directions = array<vec3f, 5>(
        vec3f(0.0, 0.0, 1.0),
        normalize(vec3f(0.8, 0.0, 1.0)), normalize(vec3f(-0.8, 0.0, 1.0)),
        normalize(vec3f(0.0, 0.8, 1.0)), normalize(vec3f(0.0, -0.8, 1.0)));
    let origin = world + normal * (frame.vct_voxel0 * frame.vct_cone_offset);
    var gi = vec3f(0.0);
    for (var cone = 0u; cone < 5u; cone++) {
        gi += trace_cone(origin, normalize(basis * directions[cone]), 0.57735);
    }
    gi /= 5.0;
    if (frame.spec_enabled != 0u) {
        let incident = normalize(world - frame.eye);
        gi += trace_cone(origin, normalize(reflect(incident, normal)), 0.12) * 0.2;
    }
    textureStore(gi_raw_out, vec2i(invocation.xy), vec4f(gi, 1.0));
}
