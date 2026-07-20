# Lantern

Lantern is Dusklight's modular multiplayer layer. The initial playable release covers the P0 core
and P1 Twilight Princess presence milestones:

- `lantern_core.dusk` owns networking, rooms, manifests, persistent settings, compatibility
  negotiation, and the public C ABI in `include/lantern/service.h`.
- `lantern_tp.dusk` registers the required `dev.twilitrealm.lantern.tp.presence` module. It sends
  quantized poses at the game's 30 Hz simulation rate, interpolates remote players, renders mod-owned Link/Wolf model clones,
  and draws colored names with one `GfxService` draw type and a bundled font atlas.
- `lantern_server.exe` is an optional game-agnostic relay. It assigns peer IDs, routes opaque module
  messages, enforces manifest compatibility and queue/rate limits, and owns no gameplay state.

No Dusklight runtime or SDK API changes are required. Lantern Core uses public services only;
Lantern TP additionally uses the public `game` and `webgpu` features plus `HookService`.

## Build

From a Visual Studio developer shell on Windows x64:

```powershell
cmake -S mods/lantern -B build/lantern -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build/lantern --target `
  lantern_core_package lantern_tp_package lantern_server -j 8
ctest --test-dir build/lantern --output-on-failure
```

The standalone build pins GameNetworkingSockets 1.5.1 and protobuf 21.12. The in-tree aggregate
build includes both packages in `dusklight_mods` on Windows.

Copy `build/lantern/mods/lantern_core.dusk` and `lantern_tp.dusk` to:

```text
%APPDATA%\TwilitRealm\Dusklight\mods
```

Both packages are required for TP presence. You can instead launch Dusklight with `--mods <dir>`.

## Host or join a room

The relay listens on UDP port `43384` by default:

```powershell
lantern_server.exe --port 43384
```

Open Lantern Core's mod panel in each client, set the server address and display details, then use
**Create room** on one client. Copy its 20-character invite code into the other clients and select
**Join room**. For internet play, allow/forward the relay's UDP port; clients do not need inbound
ports.

## Module compatibility

Lantern only reports modules that voluntarily register with `LanternService`; it does not claim
that arbitrary enabled Dusklight mods are multiplayer-compatible. A required module must exist on
both peers with a compatible major/minor range and compatibility tag. Cosmetic/client-only modules
never block a room, while optional modules exchange messages only with compatible peers. Unloading
or reloading a required module renegotiates the manifest and disconnects it if the room would become
incompatible.

All third-party callbacks are delivered from `mod_update` on the game thread within a two
millisecond drain budget. The transport thread only copies bounded frames into queues. Reliable
ordered messages carry control/module events; unreliable latest-wins messages carry pose snapshots.

## Scope after P1

Shared progression is intentionally the next, separate Lantern TP module. Its save/seed tag,
idempotent journal, relay snapshot, safe-point application, and echo suppression are not enabled in
this presence release. Distribution IDs are already reserved in manifests, but Lantern never
downloads or installs peer-provided code. Any future distribution flow must use a trusted registry,
signatures/hashes, and explicit confirmation.
