# perf_lod_plugin

A UEVR C++ plugin with two halves:

1. **Frame profiler** — tells you where wall-clock frame time actually goes,
   from inside the process. This matters because UE Shipping builds compile out
   the STATS system, so `stat unit` usually does **not** work in a retail game.
   Without it you are guessing from Task Manager, and Task Manager cannot see a
   single-threaded bottleneck.
2. **Simulation LOD** — throttles skeletal mesh animation work for actors
   outside your attention cone, once the profiler says the game thread is
   actually the thing holding you back.

Build it, run the profiler first, and only turn on the LOD half if the verdict
says the game thread is the bottleneck.

## Building

The plugin is a target in the UEVR build, so it comes along with a normal build:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target perf_lod_plugin
```

Output: `build/bin/perf_lod_plugin/perf_lod_plugin.dll` (path varies by generator).

If cmkr tries to regenerate `CMakeLists.txt` and you would rather it did not,
configure with `-DCMKR_SKIP_GENERATION=ON`. `cmake.toml` and `CMakeLists.txt`
are both updated here, so either path produces the same target.

## Installing

Copy the DLL into either:

- `%APPDATA%\UnrealVRMod\<GameExeName>\plugins\` — this game only
- `%APPDATA%\UnrealVRMod\UEVR\plugins\` — every game

## Using it

| Key | Action |
| --- | --- |
| F9  | toggle simulation LOD |
| F10 | toggle per-frame CSV capture |
| F11 | toggle the plugin window |

The window has three sections: **Frame timing**, **Object census**, and
**Simulation LOD**. Settings persist to
`%APPDATA%\UnrealVRMod\<GameExeName>\perf_lod_plugin.cfg` via the Save button
(plain `key=value`, safe to hand-edit).

### Step 1 — read the verdict, change nothing

Walk into the area that drops frames and read the Frame timing section:

```
FPS 54.3   frame 18.42 ms (p99 24.10)
game thread  17.90 ms (p99 23.60)  = 97% of frame
outside tick  0.52 ms   slate/render 3.21 ms
```

- **`game_ms` ≈ `frame_ms`** → game thread bound. The tick itself is the frame.
  Simulation LOD is worth trying.
- **`game_ms` well below `frame_ms`** → the frame is mostly spent *outside* the
  tick: GPU, render/RHI thread, or a compositor wait. No amount of gameplay
  throttling will help. Go change UEVR's **Synchronization Mode** (Early / Late
  / Very Late), **Rendering Method**, and `r.OneFrameThreadLag` instead.
- **In between** → mixed. Expect partial wins.

`Capture per-frame CSV` (F10) writes
`%APPDATA%\UnrealVRMod\<GameExeName>\perf_lod_frames.csv` with one row per
frame (`frame,frame_ms,game_ms,slate_ms,lod_ms`) so you can look at the drops
afterwards rather than trying to read numbers mid-fight.

Note on the numbers: `slate_ms` is measured on the render thread and is a
proxy, not a full render-thread cost. `outside_tick` is the honest signal — it
is time the game thread spent not ticking.

### Step 2 — blanket optimization (no gaze needed)

In the Simulation LOD section, leave **Cone throttle** off and enable only:

- `Force bEnableUpdateRateOptimizations`
- `Force VisibilityBasedAnimTickOption` → `OnlyTickPoseWhenRendered`

This is UE's own update-rate optimization, forced on for every skeletal mesh
that is not the player's. It needs no eye tracking, no cone, and no per-frame
math, and in crowd-heavy areas it is usually the larger win of the two. Toggle
it with F9 and watch `game_ms`.

If `OnlyTickPoseWhenRendered` causes visible pops when things come back into
view, step down to `OnlyTickMontagesWhenNotRendered`.

### Step 3 — cone throttle

Now enable **Cone throttle**. Everything outside the cone gets its
`TickInterval` raised (default 33 ms ≈ 30 Hz); everything inside keeps ticking
every frame. Tunables that matter:

- **Cone half angle** — start at 60°, which is roughly "in front of me".
- **Never throttle within** — a radius around you where nothing is ever
  throttled, so a companion standing next to you never stutters.
- **Components per frame** — how many components are evaluated each tick.
  This is the plugin's own cost knob; `this plugin` in the Frame timing section
  shows what it is costing you. If that number is not much smaller than what
  you are saving, turn the budget down.
- **Hysteresis** — stops components on the cone edge from flipping tick rate
  every frame as you turn your head.

The player's own mesh is never touched (any component whose Outer is the local
pawn is skipped).

### Step 4 — eye gaze (optional)

The cone follows the **camera forward** vector by default, which requires no
eye tracking and captures most of the win — the savings are in the
behind-you/far-peripheral set, and eyes only add roughly ±20° of refinement.

To drive it with real gaze, set **Source** to `Eye gaze (custom event)` and have
any other plugin (or a patched UEVR build) dispatch:

```cpp
uevr::API::get()->dispatch_custom_event("perf_lod_gaze", "0.12,-0.03,-0.99");
```

The payload is a head-space direction in OpenXR convention (+X right, +Y up,
−Z forward) as `x,y,z`. Data older than 200 ms is treated as stale and the
plugin falls back to camera forward, so losing tracking degrades instead of
breaking.

Stock UEVR does not request `XR_EXT_eye_gaze_interaction`, so gaze is not
available to plugins out of the box. [elliotttate/UEVR-VRS](https://github.com/elliotttate/UEVR-VRS)
is a UEVR *fork* that adds it (`XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME` in
`VR.cpp`'s `wanted_extensions`, a gaze action in UEVR's own action set, and
`OpenXR::get_eye_gaze_direction()`). If you merge that work, publishing gaze to
this plugin is a few lines wherever you already read it per frame:

```cpp
if (const auto gaze = openxr->get_eye_gaze_direction(); gaze.has_value()) {
    char payload[64]{};
    snprintf(payload, sizeof(payload), "%.4f,%.4f,%.4f", gaze->x, gaze->y, gaze->z);
    PluginLoader::get()->dispatch_custom_event("perf_lod_gaze", payload);
}
```

**Sanity check after wiring it up:** set the cone very narrow (~20°), look
hard left without turning your head, and confirm the throttling follows your
eyes and not your head. If it moves the wrong way, flip `Gaze yaw sign` /
`Gaze pitch sign` — the mapping between OpenXR's right-handed Y-up space and
UE's left-handed Z-up rotator is easy to get backwards, and these exist so you
can settle it empirically in ten seconds instead of reasoning about it.

## How it works

Everything expensive is resolved once, at first tick:

- `USkinnedMeshComponent::PrimaryComponentTick` → `FTickFunction::TickInterval`
  offsets, so the per-frame write is `*(float*)(component + a + b) = interval`
  with no reflection.
- `bEnableUpdateRateOptimizations` as a cached `FBoolProperty` (bitfield-safe).
- `VisibilityBasedAnimTickOption` as a cached byte offset.
- `K2_GetComponentLocation` as a cached `UFunction*`, called through
  `process_event` with a pre-sized parameter buffer. Both UE4 (float) and UE5
  (double) `FVector` layouts are detected from the return property.

Components are visited round-robin against a per-frame budget rather than all
at once, and the candidate list is refreshed on a timer from
`UObjectHook::get_objects_by_class` (an O(1) map lookup that already includes
subclasses).

Every component's original values are captured before the first write, and
restored when you disable the plugin, press "Restore everything now", or
toggle off with F9.

## Limitations

- **Tick enable/disable is deliberately not used.** `FTickFunction::bTickEnabled`
  is not a plain flag — enabling/disabling moves the tick function between the
  tick manager's lists, so poking the bit in memory does nothing useful.
  `TickInterval` is the knob that works via a raw write.
- **Pointer recycling.** Original values are keyed by `UObject*`. Destroyed
  objects are pruned on each candidate refresh (default 500 ms), which bounds
  but does not eliminate the window where the engine could recycle an address.
  Shorten the refresh interval if you see anything odd.
- **`AnimUpdateRateManager` is not reachable.** `FAnimUpdateRateParameters` is
  not a `UPROPERTY`, so it cannot be reached by name through reflection. The
  properties used here are the reflected equivalents.
- The profiler measures the **engine tick**, not individual UE stat counters. It
  tells you *whether* the game thread is the problem, not which system inside it
  is. For that, the object census plus toggling the throttle is the practical
  next step.
