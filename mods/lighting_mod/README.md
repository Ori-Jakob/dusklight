# Clustered Lighting + GI

`lighting_mod` gathers the live environment light lists after opaque actors have registered their
lights, then shades the resolved scene through a GPU clustered-deferred pass. It is disabled by
default and does not alter save data.

## Render paths

- Direct lighting: 16 x 9 x 24 froxels, 63 local lights per cluster, full- or half-resolution HDR
  shading, conservative spotlight-cone culling, and an additive soft-knee composite.
- SSGI: half-resolution 2-4 ray trace, camera reprojection with depth rejection and YCoCg variance
  clipping, three edge-aware atrous passes, and depth-guided upsampling.
- VCT: lazily allocated three-cascade 64-cubed toroidal clipmap (50/200/800 unit voxels), atomic
  screen injection, decay, mip generation, five diffuse cones, and the shared temporal filter.
- Native suppression: Off, Actors, or Full. All hooks read live config and return to the original
  game behavior when the effect is disabled.

If the Graphics Hub `dev.automata.depth_to_normal` v1.0 service is present, the mod reuses its
frame-valid world normals. The dependency is optional; standalone installs use the internal 5-tap
normal reconstruction. VCT clipmap resources are not allocated until VCT is selected.

## Debug views

Light Splats verifies gathering without resolving the scene. The remaining views show reconstructed
normals, cluster occupancy, direct light, GI, the albedo proxy, and logarithmic depth slices.

Suggested checks are Ordon/Kakariko torches at night, Goron Mines spotlights, Faron twilight fog,
and a bright outdoor daytime area. Toggle suppression through Off/Actors/Full while stationary to
confirm vanilla lighting restores live; compare SSGI and VCT while moving a light off-screen.

## Known constraints

This is a deferred opaque-scene effect: translucent geometry keeps vanilla lighting. Temporal SSGI
has camera reprojection but no per-object motion vectors, so fast actors can retain short-lived
ghosting. The VCT capture is screen-injected rather than a second world render, and its opacity decay
is intentionally used to age unseen geometry.
