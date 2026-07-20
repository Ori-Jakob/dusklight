@group(0) @binding(0) var<uniform> frame: FrameUniforms;
@group(0) @binding(1) var scene_depth: texture_2d<f32>;
@group(0) @binding(2) var normals_tex: texture_2d<f32>;
@group(0) @binding(3) var<storage, read> light_list: LightList;

struct ClusterBuffer {
    values: array<u32>,
}
@group(0) @binding(4) var<storage, read> clusters: ClusterBuffer;
@group(0) @binding(5) var light_hdr_out: texture_storage_2d<rgba16float, write>;

fn evaluate_light(light: GpuLight, world_pos: vec3f, normal: vec3f) -> vec3f {
    var direction_to_light: vec3f;
    var attenuation: f32;
    if (light.light_type == LIGHT_TYPE_DIRECTIONAL) {
        direction_to_light = normalize(light.dir_ws);
        attenuation = 1.0;
    } else {
        let offset = light.pos_ws - world_pos;
        let distance = length(offset);
        if (distance >= light.radius || distance <= 1.0e-4) {
            return vec3f(0.0);
        }
        direction_to_light = offset / distance;
        attenuation = light_falloff(frame, distance, light.radius) *
                      spot_factor(light, -direction_to_light);
    }

    let diffuse = max(dot(normal, direction_to_light), 0.0);
    var response = diffuse;
    if (frame.spec_enabled != 0u && diffuse > 0.0) {
        let view_dir = normalize(frame.eye - world_pos);
        let halfway = normalize(view_dir + direction_to_light);
        response += pow(max(dot(normal, halfway), 0.0), 24.0) * 0.12;
    }
    return light.color * light.intensity * attenuation * response;
}

@compute @workgroup_size(8, 8, 1)
fn shade_lights(@builtin(global_invocation_id) invocation: vec3u) {
    let out_size = textureDimensions(light_hdr_out);
    if (any(invocation.xy >= out_size)) {
        return;
    }
    let full_size = textureDimensions(scene_depth);
    let full_px = min(vec2u((vec2f(invocation.xy) + 0.5) * vec2f(full_size) / vec2f(out_size)),
                      full_size - vec2u(1));
    let depth = textureLoad(scene_depth, vec2i(full_px), 0).x;
    if (is_sky(depth)) {
        textureStore(light_hdr_out, vec2i(invocation.xy), vec4f(0.0));
        return;
    }

    let uv = (vec2f(full_px) + 0.5) / vec2f(full_size);
    let world_pos = unproject(frame.world_from_proj, ndc_from_uv(uv, depth));
    let normal = normalize(textureLoad(normals_tex, vec2i(full_px), 0).xyz);
    var radiance = vec3f(0.0);
    for (var i = 0u; i < min(light_list.directional_count, light_list.count); i++) {
        radiance += evaluate_light(light_list.lights[i], world_pos, normal);
    }

    let view_distance = view_dist_from_depth(frame, depth);
    let cluster_index = cluster_index_for_pixel(frame, full_px, view_distance);
    let base = cluster_index * CLUSTER_STRIDE;
    let count = min(clusters.values[base], CLUSTER_STRIDE - 1u);
    for (var i = 0u; i < count; i++) {
        let light_index = clusters.values[base + 1u + i];
        radiance += evaluate_light(light_list.lights[light_index], world_pos, normal);
    }
    textureStore(light_hdr_out, vec2i(invocation.xy),
                 vec4f(radiance, f32(count) / f32(CLUSTER_STRIDE - 1u)));
}
