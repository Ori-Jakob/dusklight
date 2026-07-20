@group(0) @binding(0) var<uniform> frame: FrameUniforms;
@group(0) @binding(1) var scene_color: texture_2d<f32>;
@group(0) @binding(2) var scene_depth: texture_2d<f32>;
@group(0) @binding(3) var normals_tex: texture_2d<f32>;
@group(0) @binding(4) var light_hdr: texture_2d<f32>;
@group(0) @binding(5) var gi_raw_out: texture_storage_2d<rgba16float, write>;

fn texel_at_uv(size: vec2u, uv: vec2f) -> vec2i {
    return vec2i(min(vec2u(uv * vec2f(size)), size - vec2u(1)));
}

fn direct_at(uv: vec2f) -> vec3f {
    let size = textureDimensions(light_hdr);
    return textureLoad(light_hdr, texel_at_uv(size, uv), 0).rgb;
}

@compute @workgroup_size(8, 8, 1)
fn trace_ssgi(@builtin(global_invocation_id) invocation: vec3u) {
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
    let world_pos = unproject(frame.world_from_proj, ndc_from_uv(uv, depth));
    let normal = normalize(textureLoad(normals_tex, vec2i(full_px), 0).xyz);
    let ray_count = clamp(frame.ssgi_rays, 2u, 4u);
    var total = vec3f(0.0);
    var hits = 0.0;
    var nearest_hit = 0.0;

    for (var ray = 0u; ray < ray_count; ray++) {
        let base_seed = pcg_hash(full_px.x + full_px.y * full_size.x +
                                 frame.frame_index * 1664525u + ray * 1013904223u);
        // Cranley-Patterson-rotated R2 sequence: stable coverage with a per-frame hash rotation.
        let r1 = fract(0.754877666 * f32(ray + 1u) + rand01(base_seed));
        let r2 = fract(0.569840296 * f32(ray + 1u) + rand01(base_seed ^ 0x9E3779B9u));
        let direction = cosine_sample_hemisphere(normal, r1, r2);
        let origin = world_pos + normal * 24.0;
        var distance = 35.0;
        for (var step = 0u; step < frame.ssgi_steps; step++) {
            let sample_world = origin + direction * distance;
            let projected = frame.proj_from_view * (frame.view_from_world * vec4f(sample_world, 1.0));
            if (projected.w <= 0.0) { break; }
            let sample_uv = uv_from_ndc(projected.xy / projected.w);
            if (any(sample_uv <= vec2f(0.0)) || any(sample_uv >= vec2f(1.0))) { break; }
            let sample_px = texel_at_uv(full_size, sample_uv);
            let scene_d = textureLoad(scene_depth, sample_px, 0).x;
            if (!is_sky(scene_d)) {
                let ray_view_distance = -(frame.view_from_world * vec4f(sample_world, 1.0)).z;
                let scene_view_distance = view_dist_from_depth(frame, scene_d);
                let separation = ray_view_distance - scene_view_distance;
                if (separation > 0.0 && separation < frame.ssgi_thickness) {
                    let scene = textureLoad(scene_color, sample_px, 0).rgb;
                    total += bounce_source(frame, scene, direct_at(sample_uv));
                    hits += 1.0;
                    nearest_hit = select(min(nearest_hit, distance), distance, nearest_hit == 0.0);
                    break;
                }
            }
            distance = distance * 1.28 + 18.0;
        }
    }
    var gi = vec3f(0.0);
    if (hits > 0.0) { gi = total / hits; }
    textureStore(gi_raw_out, vec2i(invocation.xy), vec4f(gi, nearest_hit));
}
