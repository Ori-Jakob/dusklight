// Remote wolves, Midna on their back and their transformations' effects.

const { STAGES, COMMON_CVARS, tt, warp, waitStage, barrier, meetIn, connect } = require("../lib");

// The form is picked at player create, so reload; M_077 can move stages to other layers.
const spawnAs = (form, s) => [
    { op: "setForm", form },
    warp(s),
    waitStage(s),
    { op: "expectLocalForm", form },
];

// dItemNo_WEAR_ZORA_e.
const ZORA_CLOTHES = 0x31;
// 1 << TfEmitter (fx/RemoteTransformFx.hpp).
const WTOA_A = 1 << 3;
// Every instance sets the room option: whichever one owns the room pushes it.
const hideInCutscenes = [...COMMON_CVARS, tt("hide_players_in_cutscene", 1)];

// B transforms to wolf, human and wolf again; A's dummy replays each on B's ticks.
const transformFx = (name, description, serverEnv = {}) => ({
    name,
    description,
    timeoutSec: 480,
    cvars: COMMON_CVARS,
    serverEnv,
    instances: [
        {
            name: "A",
            start: STAGES.southFaron,
            steps: [
                ...connect,
                ...meetIn(STAGES.southFaron, "B"),
                ...barrier("fx-wolf", "B"),
                { op: "expectRemoteTransformFx", form: "wolf" },
                ...barrier("a-saw-wolf", "B"),
                { op: "expectRemoteTransformFx", form: "human" },
                ...barrier("a-saw-human", "B"),
                { op: "expectRemoteTransformFx", form: "wolf" },
                ...barrier("fx-done", "B"),
                { op: "quit" },
            ],
        },
        {
            name: "B",
            start: STAGES.southFaron,
            steps: [
                ...connect,
                ...meetIn(STAGES.southFaron, "A"),
                ...barrier("fx-wolf", "A"),
                { op: "transform", form: "wolf", trace: true, timeoutSec: 30 },
                ...barrier("a-saw-wolf", "A"),
                { op: "transform", form: "human", trace: true, timeoutSec: 30 },
                ...barrier("a-saw-human", "A"),
                { op: "walk", frames: 240, circle: true, async: true },
                { op: "transform", form: "wolf", trace: true, timeoutSec: 30 },
                ...barrier("fx-done", "A"),
                { op: "quit" },
            ],
        },
    ],
});

module.exports = [
    {
        name: "wolf-smoke",
        description: "solo: spawn as a wolf, walk, transform to human and back",
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "solo",
                start: STAGES.southFaron,
                steps: [
                    waitStage(STAGES.southFaron),
                    ...spawnAs("wolf", STAGES.southFaron),
                    { op: "walk", frames: 180, circle: true },
                    { op: "transform", form: "human", timeoutSec: 30 },
                    { op: "walk", frames: 90, circle: true },
                    { op: "transform", form: "wolf", timeoutSec: 30 },
                    { op: "walk", frames: 90, circle: true },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "wolf-remote",
        description: "B transforms into a wolf and back while A checks its dummy's skeleton and clips",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "waitSignal", name: "b-wolf", from: "B", timeoutSec: 90 },
                    { op: "expectRemoteForm", form: "wolf", body: "wolf", timeoutSec: 20 },
                    { op: "checkDummies", maxDist: 400 },
                    // B walks as a wolf meanwhile: no stale human upper-body clip reaches us.
                    { op: "expectCleanAnims", frames: 180 },
                    ...barrier("a-saw-wolf", "B"),
                    { op: "waitSignal", name: "b-human", from: "B", timeoutSec: 90 },
                    { op: "expectRemoteForm", form: "human", body: "human", timeoutSec: 20 },
                    { op: "checkDummies", maxDist: 400 },
                    { op: "expectCleanAnims", frames: 120 },
                    ...barrier("wolf-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "transform", form: "wolf", timeoutSec: 30 },
                    { op: "signal", name: "b-wolf" },
                    { op: "walk", frames: 450, circle: true },
                    ...barrier("a-saw-wolf", "A"),
                    { op: "transform", form: "human", timeoutSec: 30 },
                    { op: "signal", name: "b-human" },
                    { op: "walk", frames: 300, circle: true },
                    ...barrier("wolf-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "wolf-spawn-hop",
        description: "B is a wolf before A sees it: A's dummy of B is created with the wolf body in each stage",
        timeoutSec: 480,
        cvars: COMMON_CVARS,
        instances: ["A", "B"].map((name) => {
            const other = name === "A" ? "B" : "A";
            const hops = [STAGES.southFaron, STAGES.faronField];
            return {
                name,
                start: STAGES.southFaron,
                steps: [
                    waitStage(STAGES.southFaron),
                    ...spawnAs(name === "B" ? "wolf" : "human", STAGES.southFaron),
                    ...connect,
                    ...hops.flatMap((s, k) => [
                        ...(k > 0 ? [warp(s)] : []),
                        ...meetIn(s, other),
                        ...(name === "A"
                            ? [{ op: "expectRemoteForm", form: "wolf", body: "wolf", timeoutSec: 20 }]
                            : [{ op: "expectLocalForm", form: "wolf" }]),
                        ...barrier(`wolf-hop-${k}`, other),
                    ]),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "midna-ride",
        description: "B's wolf carries Midna (shadow only, drawn, off, through a transformation); A's dummy of B shows her the same way",
        timeoutSec: 480,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "waitSignal", name: "midna-shadow", from: "B", timeoutSec: 90 },
                    { op: "expectRemoteForm", form: "wolf", body: "wolf", timeoutSec: 20 },
                    { op: "expectRemoteMidna", mode: "shadow", timeoutSec: 20 },
                    ...barrier("a-saw-shadow", "B"),
                    { op: "waitSignal", name: "midna-drawn", from: "B", timeoutSec: 60 },
                    // Held while B walks; in the light world a drawn Midna is a tired one.
                    { op: "expectRemoteMidna", mode: "drawn", tired: true, frames: 180, timeoutSec: 20 },
                    { op: "checkDummies", maxDist: 400 },
                    { op: "expectCleanAnims", frames: 90 },
                    ...barrier("a-saw-drawn", "B"),
                    { op: "waitSignal", name: "midna-off", from: "B", timeoutSec: 60 },
                    { op: "expectRemoteMidna", mode: "none", timeoutSec: 20 },
                    ...barrier("a-saw-off", "B"),
                    { op: "waitSignal", name: "midna-human", from: "B", timeoutSec: 90 },
                    { op: "expectRemoteForm", form: "human", body: "human", timeoutSec: 20 },
                    { op: "expectRemoteMidna", mode: "none" },
                    ...barrier("a-saw-human", "B"),
                    { op: "waitSignal", name: "midna-back", from: "B", timeoutSec: 90 },
                    { op: "expectRemoteForm", form: "wolf", body: "wolf", timeoutSec: 20 },
                    { op: "expectRemoteMidna", mode: "drawn", frames: 60, timeoutSec: 20 },
                    ...barrier("midna-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "transform", form: "wolf", timeoutSec: 30 },
                    { op: "setMidna", ride: true, visible: false },
                    { op: "signal", name: "midna-shadow" },
                    { op: "walk", frames: 120, circle: true },
                    ...barrier("a-saw-shadow", "A"),
                    { op: "setMidna", ride: true, visible: true },
                    { op: "signal", name: "midna-drawn" },
                    { op: "walk", frames: 400, circle: true },
                    ...barrier("a-saw-drawn", "A"),
                    { op: "setMidna", ride: false },
                    { op: "signal", name: "midna-off" },
                    ...barrier("a-saw-off", "A"),
                    { op: "setMidna", ride: true, visible: true },
                    { op: "transform", form: "human", timeoutSec: 30 },
                    { op: "signal", name: "midna-human" },
                    ...barrier("a-saw-human", "A"),
                    { op: "transform", form: "wolf", timeoutSec: 30 },
                    { op: "setMidna", ride: true, visible: true },
                    { op: "signal", name: "midna-back" },
                    { op: "walk", frames: 150, circle: true },
                    ...barrier("midna-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "midna-synthetic",
        description: "B feeds A Midna poses its game does not make here (pointing upper clip, hair hand, hand shapes, junk ids, shadow only); A's dummy shows them or falls back without crashing",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "waitSignal", name: "syn-real", from: "B", timeoutSec: 90 },
                    { op: "expectRemoteMidna", mode: "drawn", timeoutSec: 20 },
                    ...barrier("a-saw-real", "B"),
                    // WAITTP under the energy dome: pointing upper clip, big hair hand, hands 0/2.
                    { op: "waitSignal", name: "syn-patch", from: "B", timeoutSec: 30 },
                    { op: "expectRemoteMidna", mode: "drawn", upper: 0x1df, hairHand: 1, leftHand: 0, rightHand: 2, frames: 30, timeoutSec: 8 },
                    ...barrier("a-saw-patch", "B"),
                    // Unknown body, face and texture ids: the idle clip and the blinking face.
                    { op: "waitSignal", name: "syn-junk", from: "B", timeoutSec: 30 },
                    { op: "expectRemoteMidna", mode: "drawn", leftHand: 0xfe, rightHand: 0xfe, maxRefused: 99, frames: 30, timeoutSec: 8 },
                    ...barrier("a-saw-junk", "B"),
                    { op: "waitSignal", name: "syn-shadow", from: "B", timeoutSec: 30 },
                    { op: "expectRemoteMidna", mode: "shadow", maxRefused: 99, frames: 30, timeoutSec: 8 },
                    ...barrier("a-saw-shadow", "B"),
                    { op: "waitSignal", name: "syn-back", from: "B", timeoutSec: 30 },
                    { op: "expectRemoteMidna", mode: "drawn", upper: 0, hairHand: 0, maxRefused: 99, frames: 30, timeoutSec: 20 },
                    ...barrier("syn-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "transform", form: "wolf", timeoutSec: 30 },
                    { op: "setMidna", ride: true, visible: true },
                    { op: "signal", name: "syn-real" },
                    ...barrier("a-saw-real", "A"),
                    {
                        op: "patchPlayerUpdate",
                        packets: 300,
                        patch: {
                            md: [1 | (1 << 2), 0x1dc, 0x1df, 0, 0x3f8, 0x399, 0 | (2 << 8)],
                            mf: [0, 192, 0],
                            ma: [0x800, -0x1000, 0, 0, 0],
                        },
                    },
                    { op: "signal", name: "syn-patch" },
                    ...barrier("a-saw-patch", "A"),
                    {
                        op: "patchPlayerUpdate",
                        packets: 300,
                        patch: { md: [1, 0x999, 0x1e2, 0x7777, 0xffff, 0x1, 0xffff], mf: [99999, -5, 7], ma: [0, 0, 0, 0, 0] },
                    },
                    { op: "signal", name: "syn-junk" },
                    ...barrier("a-saw-junk", "A"),
                    {
                        op: "patchPlayerUpdate",
                        packets: 300,
                        patch: { md: [2, 0x1dc, 0, 0, 0, 0, 0xfe | (0xfe << 8)], mf: [0, 0, 0], ma: [0, 0, 0, 0, 0] },
                    },
                    { op: "signal", name: "syn-shadow" },
                    ...barrier("a-saw-shadow", "A"),
                    // One empty patch ends the others; the packet after it puts every field back.
                    { op: "patchPlayerUpdate", packets: 1, patch: {} },
                    { op: "signal", name: "syn-back" },
                    { op: "walk", frames: 90, circle: true },
                    ...barrier("syn-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "midna-spawn-hop",
        description: "B is a wolf with Midna on its back before A sees it: A's dummy of B is created with her in each stage",
        timeoutSec: 480,
        cvars: COMMON_CVARS,
        instances: ["A", "B"].map((name) => {
            const other = name === "A" ? "B" : "A";
            const hops = [STAGES.southFaron, STAGES.faronField];
            return {
                name,
                start: STAGES.southFaron,
                steps: [
                    waitStage(STAGES.southFaron),
                    ...spawnAs(name === "B" ? "wolf" : "human", STAGES.southFaron),
                    ...(name === "B" ? [{ op: "setMidna", ride: true, visible: true }] : []),
                    ...connect,
                    ...hops.flatMap((s, k) => [
                        ...(k > 0 ? [warp(s)] : []),
                        ...meetIn(s, other),
                        ...(name === "A"
                            ? [
                                  { op: "expectRemoteForm", form: "wolf", body: "wolf", timeoutSec: 20 },
                                  { op: "expectRemoteMidna", mode: "drawn", frames: 30, timeoutSec: 20 },
                              ]
                            : [{ op: "expectLocalForm", form: "wolf" }]),
                        ...barrier(`midna-hop-${k}`, other),
                    ]),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "midna-extras",
        description: "B's Midna voices reach A's dummy of B; she stays shown off the wolf's back while B turns human; the transformation voice plays on A at the tick B's transformation shows",
        timeoutSec: 480,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "waitSignal", name: "extras-ready", from: "B", timeoutSec: 90 },
                    { op: "expectRemoteMidna", mode: "drawn", timeoutSec: 20 },
                    { op: "markRemoteMidna" },
                    ...barrier("a-marked", "B"),
                    { op: "waitSignal", name: "extras-sfx", from: "B", timeoutSec: 30 },
                    { op: "expectRemoteMidnaSince", minSfx: 2, timeoutSec: 10 },
                    // A step aside, so captures see B's wolf and Midna clear of our Link.
                    { op: "walk", frames: 30, stickX: 1, stickY: 0 },
                    { op: "wait", frames: 20 },
                    { op: "horseCamera", target: "dummy", back: 250, up: 140, side: 260, height: 60, hold: true },
                    { op: "markRemoteMidna" },
                    ...barrier("a-marked-2", "B"),
                    // B turns human: Midna stays on its back, then floats off as its shadow.
                    { op: "expectRemoteMidnaSince", minApartTicks: 1, minShownTicks: 15, timeoutSec: 60 },
                    { op: "horseCamera", release: true },
                    { op: "waitSignal", name: "extras-human", from: "B", timeoutSec: 60 },
                    { op: "expectRemoteForm", form: "human", body: "human", timeoutSec: 20 },
                    ...barrier("a-saw-apart", "B"),
                    { op: "expectRemoteSfxTiming", sound: 0x100ad, transform: true, timeoutSec: 60 },
                    { op: "waitSignal", name: "extras-wolf", from: "B", timeoutSec: 60 },
                    { op: "expectRemoteForm", form: "wolf", body: "wolf", timeoutSec: 20 },
                    ...barrier("extras-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "transform", form: "wolf", timeoutSec: 30 },
                    { op: "setMidna", ride: true, visible: true },
                    { op: "signal", name: "extras-ready" },
                    ...barrier("a-marked", "A"),
                    // Z2SE_MDN_V_HIT and a jump: through the hook on her Z2Creature
                    { op: "midnaSfx", sound: 0x501ed },
                    { op: "wait", frames: 10 },
                    { op: "midnaSfx", sound: 0x6000b, voice: false },
                    { op: "signal", name: "extras-sfx" },
                    ...barrier("a-marked-2", "A"),
                    { op: "mark", msg: "MIDNA_HOLD transform" },
                    { op: "transform", form: "human", timeoutSec: 30 },
                    { op: "signal", name: "extras-human" },
                    ...barrier("a-saw-apart", "A"),
                    { op: "transform", form: "wolf", voice: true, timeoutSec: 30 },
                    { op: "signal", name: "extras-wolf" },
                    ...barrier("extras-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "transform-fx-selftest",
        description: "solo: transformation replay planner and the tf codec",
        timeoutSec: 120,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "solo",
                start: STAGES.linksHouse,
                steps: [waitStage(STAGES.linksHouse), { op: "transformFxSelfTest" }, { op: "quit" }],
            },
        ],
    },
    transformFx("transform-fx", "B transforms both ways; the effects on A's dummy of B start, change and end on B's ticks"),
    // Playout runs further behind: the effects still start with the clip.
    transformFx("transform-fx-jitter", "transform-fx with 0-90 ms relay jitter", { TT_TEST_JITTER_MS: "90" }),
    {
        name: "transform-fx-hidden",
        description: "A's cutscene hides B's dummy while B transforms; after it A's dummy picks up the effects at B's anchor",
        timeoutSec: 420,
        cvars: hideInCutscenes,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "waitRoomState", key: "hidePlayersInCutscene", value: true, timeoutSec: 20 },
                    { op: "forceCutscene", on: true },
                    ...barrier("a-hidden", "B"),
                    { op: "waitRemoteTransformPhase", phase: "C", timeoutSec: 60 },
                    { op: "forceCutscene", on: null },
                    { op: "expectRemoteTransformFx", form: "wolf", skip: ["A", "swap", "C"], hidden: true, anchorSource: 1 },
                    ...barrier("fx-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "waitRoomState", key: "hidePlayersInCutscene", value: true, timeoutSec: 20 },
                    ...barrier("a-hidden", "A"),
                    { op: "transform", form: "wolf", timeoutSec: 30 },
                    ...barrier("fx-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "transform-fx-synthetic",
        description: "B feeds A transformation states its game does not make here (after the swap with no anchor, junk); A's dummy shows them and stops when they stop",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    // After the swap on a human body: WTOA_A, bright, at the fallback anchor.
                    { op: "waitSignal", name: "syn-post", from: "B", timeoutSec: 30 },
                    { op: "expectDummyTransformFx", emitters: WTOA_A, tev: 50, silhouette: false, anchorSource: 2, frames: 30, timeoutSec: 8 },
                    ...barrier("a-saw-post", "B"),
                    // Junk: flags masked to active, after the swap, to wolf; TEV clamped.
                    { op: "waitSignal", name: "syn-junk", from: "B", timeoutSec: 30 },
                    { op: "expectDummyTransformFx", emitters: WTOA_A, tev: 255, frames: 30, timeoutSec: 8 },
                    ...barrier("a-saw-junk", "B"),
                    { op: "waitSignal", name: "syn-back", from: "B", timeoutSec: 30 },
                    { op: "wait", frames: 20 },
                    { op: "expectNoTransformFx", frames: 60 },
                    ...barrier("syn-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "patchPlayerUpdate", packets: 300, patch: { tf: [3, 50, 1024, 800000, 800000, 800000] } },
                    { op: "signal", name: "syn-post" },
                    ...barrier("a-saw-post", "A"),
                    { op: "patchPlayerUpdate", packets: 300, patch: { tf: [255, 99999, 99999, 2147483647, 0, 0] } },
                    { op: "signal", name: "syn-junk" },
                    ...barrier("a-saw-junk", "A"),
                    // One empty patch ends the others; the packet after it puts every field back.
                    { op: "patchPlayerUpdate", packets: 1, patch: {} },
                    { op: "signal", name: "syn-back" },
                    ...barrier("syn-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "transform-fx-clothes",
        description: "B changes clothes (a model swap too): A's dummy of B shows no transformation effects",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    ...barrier("clothes", "B"),
                    { op: "expectNoTransformFx", frames: 150 },
                    { op: "expectDummyLook", clothes: ZORA_CLOTHES, hidden: false, timeoutSec: 10 },
                    ...barrier("clothes-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    ...barrier("clothes", "A"),
                    { op: "setClothes", item: ZORA_CLOTHES, timeoutSec: 30 },
                    ...barrier("clothes-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
];
