@group(0) @binding(0) var<uniform> frame: FrameUniforms;
@group(0) @binding(1) var<storage, read> light_list: LightList;

struct ClusterBuffer {
    values: array<u32>,
}
@group(0) @binding(2) var<storage, read_write> clusters: ClusterBuffer;

fn froxel_aabb(flat_index: u32) -> array<vec3f, 2> {
    let cx = flat_index % frame.cluster_dims.x;
    let yz = flat_index / frame.cluster_dims.x;
    let cy = yz % frame.cluster_dims.y;
    let cz = yz / frame.cluster_dims.y;

    let z0 = frame.near_plane * exp(f32(cz) / frame.cluster_z_scale);
    let z1 = frame.near_plane * exp(f32(cz + 1u) / frame.cluster_z_scale);
    let ndc_x0 = f32(cx) / f32(frame.cluster_dims.x) * 2.0 - 1.0;
    let ndc_x1 = f32(cx + 1u) / f32(frame.cluster_dims.x) * 2.0 - 1.0;
    let ndc_y_top = 1.0 - f32(cy) / f32(frame.cluster_dims.y) * 2.0;
    let ndc_y_bottom = 1.0 - f32(cy + 1u) / f32(frame.cluster_dims.y) * 2.0;
    let inv_x = 1.0 / frame.proj_from_view[0][0];
    let inv_y = 1.0 / frame.proj_from_view[1][1];

    let xs = vec4f(ndc_x0 * z0 * inv_x, ndc_x1 * z0 * inv_x,
                   ndc_x0 * z1 * inv_x, ndc_x1 * z1 * inv_x);
    let ys = vec4f(ndc_y_bottom * z0 * inv_y, ndc_y_top * z0 * inv_y,
                   ndc_y_bottom * z1 * inv_y, ndc_y_top * z1 * inv_y);
    let minimum = vec3f(min(min(xs.x, xs.y), min(xs.z, xs.w)),
                        min(min(ys.x, ys.y), min(ys.z, ys.w)), -z1);
    let maximum = vec3f(max(max(xs.x, xs.y), max(xs.z, xs.w)),
                        max(max(ys.x, ys.y), max(ys.z, ys.w)), -z0);
    return array<vec3f, 2>(minimum, maximum);
}

fn sphere_intersects_aabb(center: vec3f, radius: f32, bounds: array<vec3f, 2>) -> bool {
    let closest = clamp(center, bounds[0], bounds[1]);
    let delta = center - closest;
    return dot(delta, delta) <= radius * radius;
}

fn spot_cone_may_intersect(light: GpuLight, view_pos: vec3f,
                           bounds: array<vec3f, 2>) -> bool {
    if (light.light_type != LIGHT_TYPE_SPOT) { return true; }
    let center = (bounds[0] + bounds[1]) * 0.5;
    let cluster_radius = length(bounds[1] - center);
    let to_center = center - view_pos;
    let center_distance = length(to_center);
    if (center_distance <= cluster_radius || center_distance <= 1.0e-4) { return true; }
    let direction_view = normalize((frame.view_from_world * vec4f(light.dir_ws, 0.0)).xyz);
    let padding = asin(clamp(cluster_radius / center_distance, 0.0, 1.0));
    let outer_angle = acos(clamp(light.cos_outer, -1.0, 1.0));
    let padded_cosine = cos(min(outer_angle + padding, 3.14159265));
    return dot(direction_view, to_center / center_distance) >= padded_cosine;
}

@compute @workgroup_size(64, 1, 1)
fn cull_lights(@builtin(global_invocation_id) invocation: vec3u) {
    let cluster_count = frame.cluster_dims.x * frame.cluster_dims.y * frame.cluster_dims.z;
    let cluster_index = invocation.x;
    if (cluster_index >= cluster_count) {
        return;
    }
    let base = cluster_index * CLUSTER_STRIDE;
    var count = 0u;
    let bounds = froxel_aabb(cluster_index);
    for (var i = light_list.directional_count; i < min(light_list.count, MAX_LIGHTS); i++) {
        let light = light_list.lights[i];
        let view_pos = (frame.view_from_world * vec4f(light.pos_ws, 1.0)).xyz;
        if (sphere_intersects_aabb(view_pos, light.radius, bounds) &&
            spot_cone_may_intersect(light, view_pos, bounds) && count < CLUSTER_STRIDE - 1u) {
            clusters.values[base + 1u + count] = i;
            count++;
        }
    }
    clusters.values[base] = count;
}
