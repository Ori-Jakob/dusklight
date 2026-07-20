@group(0) @binding(0) var<uniform> frame: FrameUniforms;
@group(0) @binding(1) var gi_source: texture_2d<f32>;
@group(0) @binding(2) var scene_depth: texture_2d<f32>;
@group(0) @binding(3) var normals_tex: texture_2d<f32>;
@group(0) @binding(4) var gi_out: texture_storage_2d<rgba16float, write>;

fn kernel_weight(offset: i32) -> f32 {
    let weights = array<f32, 5>(1.0, 2.0, 3.0, 2.0, 1.0);
    return weights[u32(offset + 2)];
}

fn filter_atrous(px: vec2i, size: vec2u, step_size: i32) -> vec4f {
    let center = textureLoad(gi_source, px, 0);
    if (center.a <= 1.0e-7) { return vec4f(0.0); }
    let full_size = textureDimensions(scene_depth);
    let center_full = min(vec2u((vec2f(px) + 0.5) * vec2f(full_size) / vec2f(size)),
                          full_size - vec2u(1));
    let center_normal = normalize(textureLoad(normals_tex, vec2i(center_full), 0).xyz);
    let center_z = view_dist_from_depth(frame, center.a);
    var sum = vec3f(0.0);
    var total_weight = 0.0;
    for (var y = -2; y <= 2; y++) {
        for (var x = -2; x <= 2; x++) {
            let sample_px = clamp(px + vec2i(x, y) * step_size,
                                  vec2i(0), vec2i(size) - vec2i(1));
            let sample = textureLoad(gi_source, sample_px, 0);
            if (sample.a <= 1.0e-7) { continue; }
            let sample_full = min(
                vec2u((vec2f(sample_px) + 0.5) * vec2f(full_size) / vec2f(size)),
                full_size - vec2u(1));
            let sample_normal = normalize(textureLoad(normals_tex, vec2i(sample_full), 0).xyz);
            let depth_delta = abs(center_z - view_dist_from_depth(frame, sample.a));
            let depth_weight = exp(-depth_delta / max(frame.ssgi_thickness, 1.0));
            let normal_weight = pow(max(dot(center_normal, sample_normal), 0.0), 24.0);
            let weight = kernel_weight(x) * kernel_weight(y) * depth_weight * normal_weight;
            sum += sample.rgb * weight;
            total_weight += weight;
        }
    }
    return vec4f(sum / max(total_weight, 1.0e-5), center.a);
}

@compute @workgroup_size(8, 8, 1)
fn filter_step1(@builtin(global_invocation_id) invocation: vec3u) {
    let size = textureDimensions(gi_out);
    if (any(invocation.xy >= size)) { return; }
    textureStore(gi_out, vec2i(invocation.xy), filter_atrous(vec2i(invocation.xy), size, 1));
}

@compute @workgroup_size(8, 8, 1)
fn filter_step2(@builtin(global_invocation_id) invocation: vec3u) {
    let size = textureDimensions(gi_out);
    if (any(invocation.xy >= size)) { return; }
    textureStore(gi_out, vec2i(invocation.xy), filter_atrous(vec2i(invocation.xy), size, 2));
}

@compute @workgroup_size(8, 8, 1)
fn filter_step4(@builtin(global_invocation_id) invocation: vec3u) {
    let size = textureDimensions(gi_out);
    if (any(invocation.xy >= size)) { return; }
    textureStore(gi_out, vec2i(invocation.xy), filter_atrous(vec2i(invocation.xy), size, 4));
}
