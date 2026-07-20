@group(0) @binding(0) var<uniform> frame: FrameUniforms;
@group(0) @binding(1) var<storage, read> light_list: LightList;

struct SplatVertex {
    @builtin(position) position: vec4f,
    @location(0) local: vec2f,
    @location(1) color: vec3f,
    @location(2) kind: f32,
}

const QUAD: array<vec2f, 6> = array<vec2f, 6>(
    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),
    vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0));

@vertex
fn vs_splat(@builtin(vertex_index) vertex_index: u32,
            @builtin(instance_index) instance_index: u32) -> SplatVertex {
    let light = light_list.lights[instance_index];
    let corner = QUAD[vertex_index];
    var center: vec2f;
    var extent: vec2f;

    if (light.light_type == LIGHT_TYPE_DIRECTIONAL) {
        center = vec2f(-0.90, -0.84);
        extent = vec2f(0.045, 0.06);
    } else {
        let clip = frame.proj_from_view * frame.view_from_world * vec4f(light.pos_ws, 1.0);
        if (clip.w <= 0.0) {
            center = vec2f(4.0);
            extent = vec2f(0.0);
        } else {
            center = clip.xy / clip.w;
            let view_pos = frame.view_from_world * vec4f(light.pos_ws, 1.0);
            let angular = clamp(light.radius / max(-view_pos.z, 1.0), 0.012, 0.18);
            extent = vec2f(angular / max(frame.full_size.x / frame.full_size.y, 0.01), angular);
        }
    }

    var out: SplatVertex;
    out.position = vec4f(center + corner * extent, 0.0, 1.0);
    out.local = corner;
    out.color = max(light.color * max(light.intensity, 0.25), vec3f(0.08));
    out.kind = f32(light.light_type);
    return out;
}

@fragment
fn fs_splat(in: SplatVertex) -> @location(0) vec4f {
    let r = length(in.local);
    if (r > 1.0) {
        discard;
    }
    let core = 1.0 - smoothstep(0.0, 0.72, r);
    let ring = 1.0 - smoothstep(0.035, 0.085, abs(r - 0.82));
    var color = in.color * (0.35 + core * 1.5) + vec3f(ring);
    if (in.kind > 0.5 && in.kind < 1.5) {
        let cone = 1.0 - smoothstep(0.03, 0.10, abs(in.local.x) - (in.local.y + 1.0) * 0.35);
        color += vec3f(0.15, 0.45, 1.0) * cone * 0.7;
    }
    let alpha = clamp(0.32 + core * 0.45 + ring * 0.65, 0.0, 0.92);
    return vec4f(color, alpha);
}
