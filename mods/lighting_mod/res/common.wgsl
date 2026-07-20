// Shared WGSL for the clustered lighting mod. Prepended to every pass module by the C++ loader,
// so it may only contain struct and function definitions - no bindings.

// Mirror of FrameUniforms in src/common.hpp (keep in sync; 624 bytes).
struct FrameUniforms {
    proj_from_view: mat4x4f,
    view_from_proj: mat4x4f,
    view_from_world: mat4x4f,
    world_from_view: mat4x4f,
    world_from_proj: mat4x4f,
    prev_proj_from_world: mat4x4f,
    eye: vec3f,
    frame_index: u32,
    full_size: vec2f,
    inv_full_size: vec2f,
    half_size: vec2f,
    inv_half_size: vec2f,
    near_plane: f32,
    far_plane: f32,
    cluster_z_scale: f32,
    light_count: u32,
    cluster_dims: vec3u,
    gi_mode: u32,
    tile_px: vec2f,
    ambient_estimate: f32,
    master_intensity: f32,
    sun_strength: f32,
    knee: f32,
    gi_weight: f32,
    spec_enabled: u32,
    ssgi_rays: u32,
    ssgi_steps: u32,
    ssgi_thickness: f32,
    temporal_alpha: f32,
    units_per_meter: f32,
    debug_view: u32,
    vct_decay: f32,
    vct_cone_offset: f32,
    vct_origin0: vec3f,
    vct_voxel0: f32,
    vct_origin1: vec3f,
    vct_voxel1: f32,
    vct_origin2: vec3f,
    vct_voxel2: f32,
    vct_prev_origin0: vec3f,
    vct_pad0: f32,
    vct_prev_origin1: vec3f,
    vct_pad1: f32,
    vct_prev_origin2: vec3f,
    vct_pad2: f32,
}

const LIGHT_TYPE_POINT: u32 = 0u;
const LIGHT_TYPE_SPOT: u32 = 1u;
const LIGHT_TYPE_DIRECTIONAL: u32 = 2u;

const MAX_LIGHTS: u32 = 256u;
const CLUSTER_STRIDE: u32 = 64u;  // [count, 63 light indices] per froxel

// Mirror of GpuLight in src/common.hpp (64 bytes).
struct GpuLight {
    pos_ws: vec3f,
    radius: f32,
    color: vec3f,
    intensity: f32,
    dir_ws: vec3f,
    light_type: u32,
    cos_inner: f32,
    cos_outer: f32,
    pad0: f32,
    pad1: f32,
}

// Directionals occupy indices [0, directional_count); only the rest are clustered.
struct LightList {
    count: u32,
    directional_count: u32,
    pad0: u32,
    pad1: u32,
    lights: array<GpuLight, 256>,
}

fn unproject(m: mat4x4f, ndc: vec3f) -> vec3f {
    let p = m * vec4f(ndc, 1.0);
    return p.xyz / p.w;
}

fn ndc_from_uv(uv: vec2f, depth: f32) -> vec3f {
    // WebGPU framebuffer y is down; reversed-Z depth goes straight into ndc z.
    return vec3f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth);
}

fn uv_from_ndc(ndc: vec2f) -> vec2f {
    return vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

fn is_sky(depth: f32) -> bool {
    // Reversed-Z: far plane resolves to ~0.
    return depth <= 1.0e-7;
}

// Positive view-space distance from a reversed-Z depth value, via the projection terms.
// clip.z = m[2][2]*vz + m[3][2], clip.w = -vz  =>  vz = -m[3][2] / (d + m[2][2]).
fn view_dist_from_depth(u: FrameUniforms, d: f32) -> f32 {
    let vz = -u.proj_from_view[3][2] / (d + u.proj_from_view[2][2]);
    return -vz;
}

fn cluster_slice(u: FrameUniforms, view_dist: f32) -> u32 {
    let z = max(view_dist, u.near_plane * 1.0001);
    let s = log(z / u.near_plane) * u.cluster_z_scale;
    return min(u32(max(s, 0.0)), u.cluster_dims.z - 1u);
}

fn cluster_index_for_pixel(u: FrameUniforms, px: vec2u, view_dist: f32) -> u32 {
    let cx = min(u32(f32(px.x) / u.tile_px.x), u.cluster_dims.x - 1u);
    let cy = min(u32(f32(px.y) / u.tile_px.y), u.cluster_dims.y - 1u);
    let cz = cluster_slice(u, view_dist);
    return (cz * u.cluster_dims.y + cy) * u.cluster_dims.x + cx;
}

// Windowed inverse-square falloff. Distances normalized to ~meters so intensities stay sane.
fn light_falloff(u: FrameUniforms, dist: f32, radius: f32) -> f32 {
    let x = clamp(1.0 - pow(dist / max(radius, 1.0), 4.0), 0.0, 1.0);
    let window = x * x;
    let dm = dist / u.units_per_meter;
    return window / (dm * dm + 1.0);
}

fn spot_factor(light: GpuLight, dir_to_frag: vec3f) -> f32 {
    if (light.light_type != LIGHT_TYPE_SPOT) {
        return 1.0;
    }
    let cd = dot(dir_to_frag, light.dir_ws);
    return smoothstep(light.cos_outer, light.cos_inner, cd);
}

fn luminance(c: vec3f) -> f32 {
    return dot(c, vec3f(0.2126, 0.7152, 0.0722));
}

fn to_linear(c: vec3f) -> vec3f {
    return pow(max(c, vec3f(0.0)), vec3f(2.2));
}

fn to_gamma(c: vec3f) -> vec3f {
    return pow(max(c, vec3f(0.0)), vec3f(1.0 / 2.2));
}

// Bounce source: how much light a visible pixel is throwing back into the scene.
// Scene color carries baked/ambient light; light_hdr adds the mod's own direct lighting.
fn bounce_source(u: FrameUniforms, scene: vec3f, light_hdr: vec3f) -> vec3f {
    let lin = to_linear(scene);
    let albedo = lin / max(u.ambient_estimate, 0.08);
    return lin + albedo * light_hdr;
}

// PCG hash + [0,1) floats for per-pixel noise.
fn pcg_hash(v: u32) -> u32 {
    var state = v * 747796405u + 2891336453u;
    let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

fn rand01(seed: u32) -> f32 {
    return f32(pcg_hash(seed) & 0x00FFFFFFu) / 16777216.0;
}

// Build an orthonormal basis around n (Duff et al.).
fn basis_from_normal(n: vec3f) -> mat3x3f {
    let s = select(-1.0, 1.0, n.z >= 0.0);
    let a = -1.0 / (s + n.z);
    let b = n.x * n.y * a;
    let t = vec3f(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    let bt = vec3f(b, s + n.y * n.y * a, -n.y);
    return mat3x3f(t, bt, n);
}

fn cosine_sample_hemisphere(n: vec3f, r1: f32, r2: f32) -> vec3f {
    let phi = 6.28318530718 * r1;
    let sr = sqrt(r2);
    let local = vec3f(sr * cos(phi), sr * sin(phi), sqrt(max(0.0, 1.0 - r2)));
    return normalize(basis_from_normal(n) * local);
}

// Inferno-ish ramp for heatmaps.
fn heat_color(t: f32) -> vec3f {
    let x = clamp(t, 0.0, 1.0);
    return vec3f(
        clamp(1.5 * x, 0.0, 1.0),
        clamp(1.5 * x - 0.5, 0.0, 1.0) * clamp(2.0 - 2.0 * x, 0.0, 1.0) +
            clamp(3.0 * x - 2.4, 0.0, 1.0),
        clamp(3.0 * x - 2.25, 0.0, 1.0));
}

// ---- VCT clipmap helpers ----

const VCT_DIM: u32 = 64u;
const VCT_CASCADES: u32 = 3u;

fn vct_origin(u: FrameUniforms, cascade: u32) -> vec3f {
    if (cascade == 0u) { return u.vct_origin0; }
    if (cascade == 1u) { return u.vct_origin1; }
    return u.vct_origin2;
}

fn vct_voxel_size(u: FrameUniforms, cascade: u32) -> f32 {
    if (cascade == 0u) { return u.vct_voxel0; }
    if (cascade == 1u) { return u.vct_voxel1; }
    return u.vct_voxel2;
}

// World-space cell coordinate (integer lattice, may be negative) of a position in a cascade.
fn vct_world_cell(u: FrameUniforms, cascade: u32, world: vec3f) -> vec3i {
    return vec3i(floor(world / vct_voxel_size(u, cascade)));
}

// True when the cell lies inside the cascade's current 64^3 window.
fn vct_in_window(u: FrameUniforms, cascade: u32, cell: vec3i) -> bool {
    let origin = vec3i(vct_origin(u, cascade));
    let rel = cell - origin;
    return all(rel >= vec3i(0)) && all(rel < vec3i(i32(VCT_DIM)));
}

// Toroidal texel for a world cell.
fn vct_texel(cell: vec3i) -> vec3u {
    let m = cell % vec3i(i32(VCT_DIM));
    return vec3u((m + vec3i(i32(VCT_DIM))) % vec3i(i32(VCT_DIM)));
}

// Toroidal uvw for trilinear sampling with a repeat sampler.
fn vct_uvw(u: FrameUniforms, cascade: u32, world: vec3f) -> vec3f {
    return world / (vct_voxel_size(u, cascade) * f32(VCT_DIM));
}

// Flat index of a voxel in the per-cascade state/accum arrays.
fn vct_flat(cascade: u32, texel: vec3u) -> u32 {
    let base = cascade * VCT_DIM * VCT_DIM * VCT_DIM;
    return base + (texel.z * VCT_DIM + texel.y) * VCT_DIM + texel.x;
}

// Tag identifying which world cell a toroidal slot currently holds (10 bits/axis).
fn vct_tag(cell: vec3i) -> u32 {
    let c = vec3u(cell & vec3i(0x3FF));
    return (c.x << 20u) | (c.y << 10u) | c.z | 0x40000000u;  // bit 30: valid
}
