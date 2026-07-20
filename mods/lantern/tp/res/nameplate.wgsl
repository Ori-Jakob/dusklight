struct LabelUniforms {
    anchor_and_cell: vec4f,
    color: vec4f,
    glyph_count: u32,
    padding_0: u32,
    padding_1: u32,
    padding_2: u32,
}

@group(0) @binding(0) var<uniform> label: LabelUniforms;
@group(0) @binding(1) var<storage, read> glyphs: array<u32>;
@group(0) @binding(2) var font_atlas: texture_2d<f32>;
@group(0) @binding(3) var font_sampler: sampler;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) vertex: u32, @builtin(instance_index) instance: u32) -> VertexOutput {
    let corners = array<vec2f, 6>(
        vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0),
        vec2f(0.0, 1.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0));
    let corner = corners[vertex];
    let cell = label.anchor_and_cell.zw;
    let start_x = label.anchor_and_cell.x - (f32(label.glyph_count) * cell.x * 0.5);
    let screen = vec2f(start_x + (f32(instance) + corner.x) * cell.x,
        label.anchor_and_cell.y + corner.y * cell.y);

    let glyph = glyphs[instance];
    let atlas_cell = vec2f(f32(glyph % 8u), f32(glyph / 8u));
    let atlas_size = vec2f(48.0, 48.0);
    let uv = (atlas_cell * vec2f(6.0, 8.0) + corner * vec2f(6.0, 8.0)) / atlas_size;

    var out: VertexOutput;
    out.position = vec4f(screen.x * 2.0 - 1.0, 1.0 - screen.y * 2.0, 0.0, 1.0);
    out.uv = uv;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let coverage = textureSample(font_atlas, font_sampler, in.uv).r;
    if coverage < 0.25 {
        discard;
    }
    return vec4f(label.color.rgb, label.color.a * coverage);
}
