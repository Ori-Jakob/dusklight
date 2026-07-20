@group(0) @binding(0) var<uniform> frame: FrameUniforms;
@group(0) @binding(1) var gi_raw: texture_2d<f32>;
@group(0) @binding(2) var gi_history: texture_2d<f32>;
@group(0) @binding(3) var scene_depth: texture_2d<f32>;
@group(0) @binding(4) var gi_history_out: texture_storage_2d<rgba16float, write>;

fn rgb_to_ycocg(rgb: vec3f) -> vec3f {
    return vec3f(0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b,
                 0.5 * rgb.r - 0.5 * rgb.b,
                 -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b);
}

fn ycocg_to_rgb(v: vec3f) -> vec3f {
    return vec3f(v.x + v.y - v.z, v.x + v.z, v.x - v.y - v.z);
}

fn raw_clamped(px: vec2i, size: vec2u) -> vec3f {
    return textureLoad(gi_raw, clamp(px, vec2i(0), vec2i(size) - vec2i(1)), 0).rgb;
}

@compute @workgroup_size(8, 8, 1)
fn filter_temporal(@builtin(global_invocation_id) invocation: vec3u) {
    let size = textureDimensions(gi_history_out);
    if (any(invocation.xy >= size)) { return; }
    let full_size = textureDimensions(scene_depth);
    let full_px = min(vec2u((vec2f(invocation.xy) + 0.5) * vec2f(full_size) / vec2f(size)),
                      full_size - vec2u(1));
    let depth = textureLoad(scene_depth, vec2i(full_px), 0).x;
    if (is_sky(depth)) {
        textureStore(gi_history_out, vec2i(invocation.xy), vec4f(0.0));
        return;
    }

    let current = textureLoad(gi_raw, vec2i(invocation.xy), 0).rgb;
    var mean = vec3f(0.0);
    var second_moment = vec3f(0.0);
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            let value = rgb_to_ycocg(raw_clamped(vec2i(invocation.xy) + vec2i(x, y), size));
            mean += value;
            second_moment += value * value;
        }
    }
    mean /= 9.0;
    let sigma = sqrt(max(second_moment / 9.0 - mean * mean, vec3f(0.0)));

    let uv = (vec2f(full_px) + 0.5) / vec2f(full_size);
    let world = unproject(frame.world_from_proj, ndc_from_uv(uv, depth));
    let prev_clip = frame.prev_proj_from_world * vec4f(world, 1.0);
    var history_valid = prev_clip.w > 0.0;
    let prev_uv = uv_from_ndc(prev_clip.xy / max(prev_clip.w, 1.0e-8));
    history_valid = history_valid && all(prev_uv > vec2f(0.0)) && all(prev_uv < vec2f(1.0));
    var history = vec4f(0.0);
    if (history_valid) {
        let history_size = textureDimensions(gi_history);
        let history_px = vec2i(min(vec2u(prev_uv * vec2f(history_size)), history_size - vec2u(1)));
        history = textureLoad(gi_history, history_px, 0);
        let expected_depth = prev_clip.z / prev_clip.w;
        history_valid = history.a > 1.0e-7 &&
            abs(view_dist_from_depth(frame, expected_depth) -
                view_dist_from_depth(frame, history.a)) < frame.ssgi_thickness * 2.0;
    }

    var result = current;
    if (history_valid) {
        let history_ycocg = rgb_to_ycocg(history.rgb);
        let clipped = clamp(history_ycocg, mean - 1.5 * sigma, mean + 1.5 * sigma);
        result = mix(ycocg_to_rgb(clipped), current, frame.temporal_alpha);
    } else {
        result = mix(vec3f(0.0), current, 0.5);
    }
    textureStore(gi_history_out, vec2i(invocation.xy), vec4f(max(result, vec3f(0.0)), depth));
}
