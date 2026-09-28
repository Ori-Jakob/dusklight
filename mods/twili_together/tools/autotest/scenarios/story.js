// Story sync. The capture: South Faron point 20 (layer 8) -> the cell -> point 24 -> point 0.

const { STAGES, COMMON_CVARS, warp, waitStage, barrier, connect } = require("../lib");

// Day 2 complete (F_0565): South Faron loads twilight layer 14.
const DAY2_DONE = 0x4510;
// Set by the wake-up in the cell (F_0630): the cell then loads layer 14.
const F_0630 = 0x4d08;
const faronDay3 = { stage: "F_SP108", room: 0, point: 0, eventBits: [DAY2_DONE] };
const houseDay3 = { ...STAGES.linksHouse, eventBits: [DAY2_DONE] };
const cell = { stage: "R_SP107" };
const southFaron = { stage: "F_SP108" };

const triggerCapture = { op: "triggerStory", via: "arrival", point: 20, layer: 8 };
// Where the capture plays: South Faron room 0 on the cutscene layer.
const toCaptureLayer = [warp({ stage: "F_SP108", room: 0, point: 0, layer: 8 }), waitStage(southFaron), { op: "wait", frames: 30 }];
// The chain after the capture: say no to the save prompt, sit through the wake-up.
const playOutCapture = [
    { op: "dismissSaveRequest", timeoutSec: 240 },
    { op: "waitStorySettled", timeoutSec: 180 },
    waitStage(cell, 60),
];
const endInCellAsWolf = [
    waitStage(cell, 120),
    { op: "waitStorySettled", timeoutSec: 120 },
    { op: "expectLocalForm", form: "wolf" },
    { op: "expectLayer", natural: true },
    { op: "expectConsistent", value: true },
];
const capturedAndSent = [
    ...playOutCapture,
    { op: "expectStoryMove", role: "sent", curated: "faron-capture", qualHas: 15, timeoutSec: 180 },
    { op: "expectLocalForm", form: "wolf" },
    { op: "expectEventBit", no: F_0630 },
];

// A (the originator) is captured; the follower's steps decide the rest.
const originator = (other) => ({
    name: "A",
    start: faronDay3,
    steps: [
        ...connect,
        waitStage(southFaron),
        { op: "waitPeers", count: 1, timeoutSec: 90 },
        ...barrier("ready", other, 180),
        triggerCapture,
        ...capturedAndSent,
        { op: "signal", name: "a-sent" },
        ...barrier("done", other, 400),
        { op: "quit" },
    ],
});
const follower = (steps) => ({
    name: "B",
    start: houseDay3,
    steps: [
        ...connect,
        waitStage(STAGES.linksHouse),
        { op: "waitPeers", count: 1, timeoutSec: 90 },
        ...barrier("ready", "A", 180),
        ...steps,
        ...barrier("done", "A", 400),
        { op: "quit" },
    ],
});

module.exports = [
    {
        name: "story-classify",
        description: "story sync self-test: qualification, curated moves, segments, spawn form, entrances, STORY_MOVE round trip",
        timeoutSec: 120,
        cvars: COMMON_CVARS,
        instances: [{ name: "solo", start: houseDay3, steps: [waitStage(STAGES.linksHouse), { op: "storySelfTest" }, { op: "quit" }] }],
    },
    {
        name: "story-capture-solo",
        description: "the capture from its natural trigger is recognised as one story move (3 hops) and sent; the player ends a wolf in the cell",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        instances: [{
            name: "solo",
            start: faronDay3,
            steps: [...connect, waitStage(southFaron), triggerCapture, ...capturedAndSent,
                { op: "expectLayer", natural: true }, { op: "quit" }],
        }],
    },
    {
        name: "story-accept-capture",
        description: "A is captured; B in Link's house gets the follow prompt, follows, and ends a wolf in the cell layer its flags pick",
        timeoutSec: 600,
        cvars: COMMON_CVARS,
        instances: [
            originator("B"),
            follower([
                { op: "expectPrompt", kind: "move", timeoutSec: 400 },
                { op: "dumpStory" },
                // Long enough to see (and screenshot) the pop-up.
                { op: "wait", sec: 4 },
                { op: "answerPrompt", answer: "follow" },
                { op: "expectStoryLoad", state: "arrived", timeoutSec: 120 },
                ...endInCellAsWolf,
                { op: "expectStoryMove", role: "none" },
            ]),
        ],
    },
    {
        name: "story-decline-walk",
        description: "the reported bug: B declines, walks to South Faron by the normal route, is offered the cell again and ends a wolf there",
        timeoutSec: 600,
        cvars: COMMON_CVARS,
        instances: [
            originator("B"),
            follower([
                { op: "expectPrompt", kind: "move", timeoutSec: 400 },
                { op: "answerPrompt", answer: "decline" },
                { op: "expectConsistent", value: false, segment: "captured", timeoutSec: 30 },
                // Stands in for the walk: South Faron by its normal entrance (point 0).
                warp(STAGES.southFaron),
                waitStage(southFaron),
                { op: "expectPrompt", kind: "inconsistent", timeoutSec: 60 },
                { op: "answerPrompt", answer: "catchup" },
                { op: "expectStoryLoad", state: "arrived", timeoutSec: 120 },
                ...endInCellAsWolf,
                { op: "expectStoryMove", role: "none" },
            ]),
        ],
    },
    {
        name: "story-decline-button",
        description: "B declines the follow prompt and catches up later with the window's Catch up to story",
        timeoutSec: 600,
        cvars: COMMON_CVARS,
        instances: [
            originator("B"),
            follower([
                { op: "expectPrompt", kind: "move", timeoutSec: 400 },
                { op: "answerPrompt", answer: "decline" },
                { op: "expectNoPrompt", sec: 3 },
                // The Players tab's Story section, for a look (and a screenshot).
                { op: "showWindow", tab: 2 },
                { op: "wait", sec: 4 },
                { op: "hideWindow" },
                { op: "catchUp", expectKind: "entrance" },
                { op: "expectStoryLoad", state: "arrived", timeoutSec: 120 },
                ...endInCellAsWolf,
            ]),
        ],
    },
    {
        name: "story-repair",
        description: "an already-stuck save (captured flags, human in South Faron) is repaired by Catch up to story: the cell, the wake-up, a wolf",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        instances: [{
            name: "solo",
            start: faronDay3,
            steps: [
                ...connect,
                { op: "storyPrompts", value: false },
                waitStage(southFaron),
                { op: "expectLocalForm", form: "human" },
                // A broken save: the teammate's capture wrote these, this player never went through it.
                { op: "setEventBit", no: F_0630 },
                { op: "setTransformLevel", level: 0 },
                { op: "expectConsistent", value: false, segment: "captured" },
                { op: "catchUp", expectKind: "entrance" },
                { op: "expectStoryLoad", state: "arrived", timeoutSec: 120 },
                ...endInCellAsWolf,
                // The wake-up's exit to point 0 is part of the repair, not a move to broadcast.
                { op: "expectStoryMove", role: "none" },
                { op: "quit" },
            ],
        }],
    },
    {
        name: "story-pullin-capture",
        description: "B stands in A's room when A's capture starts: both watch it, each other's dummy hidden, both end wolves in the cell, one move sent",
        timeoutSec: 600,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: faronDay3,
                steps: [
                    ...connect,
                    waitStage(southFaron),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    { op: "waitSignal", name: "b-here", from: "B", timeoutSec: 180 },
                    triggerCapture,
                    { op: "expectDummyHidden", hidden: true, timeoutSec: 45 },
                    ...capturedAndSent,
                    { op: "signal", name: "a-sent" },
                    ...barrier("done", "B", 400),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: faronDay3,
                steps: [
                    ...connect,
                    waitStage(southFaron),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    ...toCaptureLayer,
                    { op: "signal", name: "b-here" },
                    { op: "expectJoin", state: "running", timeoutSec: 90 },
                    { op: "expectDummyHidden", hidden: true, timeoutSec: 30 },
                    ...playOutCapture,
                    ...endInCellAsWolf,
                    { op: "waitSignal", name: "a-sent", from: "A", timeoutSec: 300 },
                    { op: "expectNoPrompt", sec: 10 },
                    { op: "expectStoryMove", role: "none" },
                    ...barrier("done", "A", 120),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "story-pullin-unsafe",
        description: "B is swimming when A's capture starts: B misses it, stays, then follows from the prompt and ends a wolf in the cell",
        timeoutSec: 600,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: faronDay3,
                steps: [
                    ...connect,
                    waitStage(southFaron),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    { op: "waitSignal", name: "b-here", from: "B", timeoutSec: 180 },
                    triggerCapture,
                    ...capturedAndSent,
                    ...barrier("done", "B", 400),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: faronDay3,
                steps: [
                    ...connect,
                    waitStage(southFaron),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    ...toCaptureLayer,
                    { op: "forcePullInBlocker", code: "swimming" },
                    { op: "signal", name: "b-here" },
                    { op: "expectJoin", state: "missed", reason: "swimming", timeoutSec: 90 },
                    { op: "forcePullInBlocker", code: null },
                    { op: "wait", sec: 5 },
                    waitStage(southFaron),
                    { op: "expectPrompt", kind: "move", timeoutSec: 400 },
                    { op: "answerPrompt", answer: "follow" },
                    { op: "expectStoryLoad", state: "arrived", timeoutSec: 120 },
                    ...endInCellAsWolf,
                    ...barrier("done", "A", 120),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "story-scope",
        description: "C on another team in A's room is neither pulled in nor prompted; teammate B in Link's house gets the prompt",
        timeoutSec: 600,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: faronDay3,
                steps: [
                    ...connect,
                    waitStage(southFaron),
                    { op: "waitPeers", count: 2, timeoutSec: 90 },
                    { op: "waitSignal", name: "b-ready", from: "B", timeoutSec: 180 },
                    { op: "waitSignal", name: "c-here", from: "C", timeoutSec: 180 },
                    triggerCapture,
                    ...capturedAndSent,
                    { op: "signal", name: "a-sent" },
                    { op: "waitSignal", name: "b-done", from: "B", timeoutSec: 300 },
                    { op: "waitSignal", name: "c-done", from: "C", timeoutSec: 300 },
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: houseDay3,
                steps: [
                    ...connect,
                    waitStage(STAGES.linksHouse),
                    { op: "waitPeers", count: 2, timeoutSec: 90 },
                    { op: "signal", name: "b-ready" },
                    { op: "expectPrompt", kind: "move", timeoutSec: 400 },
                    { op: "answerPrompt", answer: "decline" },
                    { op: "signal", name: "b-done" },
                    { op: "wait", sec: 2 },
                    { op: "quit" },
                ],
            },
            {
                name: "C",
                start: faronDay3,
                steps: [
                    { op: "connect", team: "u" },
                    { op: "waitConnected", timeoutSec: 20 },
                    waitStage(southFaron),
                    { op: "waitPeers", count: 2, timeoutSec: 90 },
                    ...toCaptureLayer,
                    { op: "signal", name: "c-here" },
                    { op: "expectJoin", state: "none", holdSec: 60 },
                    { op: "waitSignal", name: "a-sent", from: "A", timeoutSec: 400 },
                    { op: "expectNoPrompt", sec: 10 },
                    { op: "expectStoryMove", role: "notReceived" },
                    { op: "expectConsistent", value: true },
                    waitStage(southFaron),
                    { op: "signal", name: "c-done" },
                    { op: "wait", sec: 2 },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "story-sync-off",
        description: "with world sync off, B in A's room is not pulled in, gets no prompt, and Catch up to story has nothing to do",
        timeoutSec: 600,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: faronDay3,
                steps: [
                    ...connect,
                    { op: "setRoomOption", name: "syncWorldState", value: false },
                    { op: "waitRoomOption", name: "syncWorldState", value: false, timeoutSec: 30 },
                    waitStage(southFaron),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    { op: "waitSignal", name: "b-here", from: "B", timeoutSec: 180 },
                    triggerCapture,
                    ...playOutCapture,
                    { op: "expectLocalForm", form: "wolf" },
                    { op: "wait", sec: 5 },
                    { op: "expectStoryMove", role: "none" },
                    { op: "signal", name: "a-done" },
                    ...barrier("done", "B", 300),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: faronDay3,
                steps: [
                    ...connect,
                    { op: "setRoomOption", name: "syncWorldState", value: false },
                    { op: "waitRoomOption", name: "syncWorldState", value: false, timeoutSec: 30 },
                    waitStage(southFaron),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    ...toCaptureLayer,
                    { op: "signal", name: "b-here" },
                    { op: "expectJoin", state: "none", holdSec: 60 },
                    { op: "waitSignal", name: "a-done", from: "A", timeoutSec: 400 },
                    { op: "expectNoPrompt", sec: 5 },
                    { op: "expectStoryMove", role: "notReceived" },
                    { op: "catchUp", expectKind: "none" },
                    waitStage(southFaron),
                    ...barrier("done", "A", 120),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "story-late-join",
        description: "B connects after A's capture: the server's cached move brings the follow prompt; B follows and ends a wolf in the cell",
        timeoutSec: 600,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: faronDay3,
                steps: [
                    ...connect,
                    waitStage(southFaron),
                    triggerCapture,
                    ...capturedAndSent,
                    { op: "waitSignal", name: "b-followed", from: "B", timeoutSec: 480 },
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: houseDay3,
                steps: [
                    waitStage(STAGES.linksHouse),
                    // Long enough for A's whole capture chain (about 80 s after boot).
                    { op: "wait", sec: 170 },
                    ...connect,
                    { op: "expectStoryMove", role: "received", curated: "faron-capture", cached: true, timeoutSec: 60 },
                    { op: "expectPrompt", kind: "move", timeoutSec: 60 },
                    { op: "answerPrompt", answer: "follow" },
                    { op: "expectStoryLoad", state: "arrived", timeoutSec: 120 },
                    ...endInCellAsWolf,
                    { op: "signal", name: "b-followed" },
                    { op: "wait", sec: 2 },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "story-observe",
        description: "Phase 0: dumps the map events, exits, PLYR points and event tags of Ordon Spring and South Faron on Day 3",
        manualOnly: true,
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [{
            name: "solo",
            start: { stage: "F_SP104", room: 1, point: 0, eventBits: [DAY2_DONE] },
            steps: [
                waitStage({ stage: "F_SP104" }),
                { op: "dumpStory" },
                warp(STAGES.southFaron),
                { op: "waitStage", stage: "F_SP108", timeoutSec: 120 },
                { op: "dumpStory" },
                { op: "quit" },
            ],
        }],
    },
];
