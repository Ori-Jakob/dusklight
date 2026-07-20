@group(0) @binding(0) var scene_color: texture_2d<f32>;
@group(0) @binding(1) var scene_depth: texture_2d<f32>;
@group(0) @binding(2) var normals_tex: texture_2d<f32>;
@group(0) @binding(3) var light_hdr: texture_2d<f32>;
@group(0) @binding(4) var gi_filtered: texture_2d<f32>;
@group(0) @binding(5) var<uniform> frame: FrameUniforms;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> VertexOutput {
    let uv = vec2f(f32((vertex_index << 1u) & 2u), f32(vertex_index & 2u));
    var out: VertexOutput;
    out.position = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}

fn texel_for_uv(texture_size: vec2u, uv: vec2f) -> vec2i {
    return vec2i(min(vec2u(uv * vec2f(texture_size)), texture_size - vec2u(1)));
}

fn direct_at(uv: vec2f) -> vec4f {
    let light_size = textureDimensions(light_hdr);
    let full_size = textureDimensions(scene_depth);
    if (all(light_size == full_size)) {
        return textureLoad(light_hdr, texel_for_uv(light_size, uv), 0);
    }

    // Select the half-resolution sample whose source depth best matches this full-resolution
    // pixel. This preserves foreground light boundaries instead of bleeding the nearest texel.
    let target_px = texel_for_uv(full_size, uv);
    let target_depth = textureLoad(scene_depth, target_px, 0).x;
    let light_coord = (vec2f(target_px) + 0.5) * vec2f(light_size) / vec2f(full_size) - 0.5;
    let base = vec2i(floor(light_coord));
    var best_error = 1.0e30;
    var best = vec4f(0.0);
    for (var y = 0; y < 2; y++) {
        for (var x = 0; x < 2; x++) {
            let lp = clamp(base + vec2i(x, y), vec2i(0), vec2i(light_size) - vec2i(1));
            let source_full = min(
                vec2u((vec2f(lp) + 0.5) * vec2f(full_size) / vec2f(light_size)),
                full_size - vec2u(1));
            let source_depth = textureLoad(scene_depth, vec2i(source_full), 0).x;
            var error = 1.0e30;
            if (!is_sky(source_depth)) {
                error = abs(view_dist_from_depth(frame, target_depth) -
                            view_dist_from_depth(frame, source_depth));
            }
            if (error < best_error) {
                best_error = error;
                best = textureLoad(light_hdr, lp, 0);
            }
        }
    }
    return best;
}

fn gi_at(uv: vec2f) -> vec3f {
    let gi_size = textureDimensions(gi_filtered);
    if (all(gi_size == vec2u(1))) {
        return textureLoad(gi_filtered, vec2i(0), 0).rgb;
    }
    let full_size = textureDimensions(scene_depth);
    let target_px = texel_for_uv(full_size, uv);
    let target_depth = textureLoad(scene_depth, target_px, 0).x;
    let gi_coord = (vec2f(target_px) + 0.5) * vec2f(gi_size) / vec2f(full_size) - 0.5;
    let base = vec2i(floor(gi_coord));
    var best_error = 1.0e30;
    var best = vec3f(0.0);
    for (var y = 0; y < 2; y++) {
        for (var x = 0; x < 2; x++) {
            let sample_px = clamp(base + vec2i(x, y), vec2i(0), vec2i(gi_size) - vec2i(1));
            let sample = textureLoad(gi_filtered, sample_px, 0);
            if (sample.a > 1.0e-7) {
                let error = abs(view_dist_from_depth(frame, target_depth) -
                                view_dist_from_depth(frame, sample.a));
                if (error < best_error) {
                    best_error = error;
                    best = sample.rgb;
                }
            }
        }
    }
    return best;
}

fn albedo_proxy(scene: vec3f) -> vec3f {
    return to_linear(scene) / max(frame.ambient_estimate, 0.08);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let full_size = textureDimensions(scene_depth);
    let px = texel_for_uv(full_size, in.uv);
    let depth = textureLoad(scene_depth, px, 0).x;
    if (is_sky(depth)) {
        return vec4f(0.0);
    }
    if (length(textureLoad(normals_tex, px, 0).xyz) < 0.25) {
        return vec4f(0.0);
    }
    let scene = textureLoad(scene_color, px, 0).rgb;
    let albedo = albedo_proxy(scene);
    let direct = direct_at(in.uv).rgb;
    let indirect = select(vec3f(0.0), gi_at(in.uv), frame.gi_mode != 0u);
    let delta = albedo * (direct + frame.gi_weight * indirect);
    let mapped = frame.master_intensity * delta / (1.0 + luminance(delta) * frame.knee);
    return vec4f(mapped, 0.0);
}

@fragment
fn fs_debug(in: VertexOutput) -> @location(0) vec4f {
    let full_size = textureDimensions(scene_depth);
    let px = texel_for_uv(full_size, in.uv);
    let depth = textureLoad(scene_depth, px, 0).x;
    if (is_sky(depth)) {
        return vec4f(0.015, 0.02, 0.035, 1.0);
    }
    let direct = direct_at(in.uv);
    var color = vec3f(0.0);
    switch frame.debug_view {
        case 2u: {
            color = textureLoad(normals_tex, px, 0).xyz * 0.5 + 0.5;
        }
        case 3u: {
            color = heat_color(direct.a);
        }
        case 4u: {
            color = direct.rgb / (vec3f(1.0) + direct.rgb);
        }
        case 5u: {
            let gi = gi_at(in.uv);
            color = gi / (vec3f(1.0) + gi);
        }
        case 6u: {
            let scene = textureLoad(scene_color, px, 0).rgb;
            color = to_gamma(clamp(albedo_proxy(scene), vec3f(0.0), vec3f(1.0)));
        }
        case 7u: {
            let slice = cluster_slice(frame, view_dist_from_depth(frame, depth));
            color = heat_color(f32(slice) / f32(max(frame.cluster_dims.z - 1u, 1u)));
        }
        default: {
            color = direct.rgb;
        }
    }
    return vec4f(color, 1.0);
}
