# MC2FrameFix

An ASI plugin for **Midnight Club II** (PC, `mc2.exe`). It makes the physics independent of the frame rate and interpolates frames, so the game keeps its 30 Hz handling while drawing smoothly at 60, 144 Hz and higher.

## What it does

- **Fixed-timestep physics.** In races the game steps its physics once per rendered frame with the real frame time, so handling, top speed, burnout boost and crash damage change with fps. MC2FrameFix runs the simulation in fixed steps (`simHz`, default 30 Hz, the rate the game was tuned for) whatever the frame rate.
- **Interpolation.** Frames are drawn between the last two simulation steps:
  - the player's car, opponents and every other active physics object (knocked props too);
  - ambient traffic;
  - pedestrians, including their skeleton pose (arms and legs);
  - the camera, including its FOV.
- **Whole cars move together.** Wheels, shadows and other parts the game caches during the step are moved with the blended body while each car draws. Tail and head light glows follow too.
- **Camera cuts.** Camera switches, look-back and big jumps are never blended through: the new camera follows the blended car from its first frame, so a switch is as smooth as the rest.
- **Input per tick.** Input is polled once per simulation tick, so a quick tap on a frame without a tick isn't lost.
- **Draw-time animations at the right speed.** HUD, pause-menu and other animations advanced while drawing use the real frame time, as in the original game.
- **No audio-stream hitches.** City ambience and voice streams are opened on a worker thread; the original game opens them on the main thread, stalling a frame ~100 ms each time.
- **No engine-sound scratching on camera switches.** The game's Doppler effect reacted to the camera's fly-through on every switch; it is held for half a second instead.
- **No engine-sound drop-out on camera cuts.** The game briefly placed the listener where the new camera was last used; it now waits for the camera's real position.
- **LOD distance.** Cars and traffic keep their detailed models farther away (`lodDistanceScale`).
- **No crash on exit.** The game frees its debug font twice at shutdown; the second free is skipped.
- **Frame limiter** (optional).

Menus, movies and loading run exactly as in the original game.

## Requirements

- `mc2.exe` with SHA-1 `7dc19475132af0724139cb1916a44a42d5368bd8`.
- An ASI loader (Ultimate ASI Loader, Win32, named `dinput8.dll` or `version.dll`).
- To go above 60 fps you still need something that lifts the game's vsync cap: dgVoodoo2, DXVK, a forced refresh rate (3D Analyze), or your driver's settings.

Every patch site is checked against the expected bytes first. On any other exe, nothing is patched and the log says so.

## Installation

1. Put the ASI loader in the game folder.
2. Put `MC2FrameFix.asi` next to `mc2.exe` (or in the loader's `scripts` folder).
3. Start a race. `MC2FrameFix.ini` and `MC2FrameFix.log` are created next to the `.asi`.

## Settings (`MC2FrameFix.ini`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `1` | `0` = vanilla behaviour. |
| `simHz` | `30` | Simulation rate, 25–1000. `30` = console tuning, `60` = original PC feel on a 60 Hz screen. |
| `maxStepsPerFrame` | `8` | Cap on steps per frame if the PC can't keep up. |
| `fpsLimit` | `0` | Frame limiter, used with vsync **off**. Set it to your refresh rate. With vsync on, leave it at `0`. |
| `interpolate` | `1` | Draw between simulation steps. |
| `interpolateCamera` | `1` | Blend the camera. |
| `interpolateModels` | `1` | Move car and traffic parts (wheels, shadow) with the blended body. |
| `interpolateTraffic` | `1` | Ambient traffic. |
| `interpolatePhysics` | `1` | Every active physics object (cars, opponents, props). |
| `interpolatePeds` | `1` | Pedestrians. |
| `interpolateGlows` | `1` | Light glows follow the blended car. |
| `maxStepDistance` | `12` | A step longer than this (metres) counts as a teleport and isn't blended. |
| `maxStepAngle` | `60` | Same, for rotation (degrees per step). |
| `maxCameraAngle` | `45` | A camera turning more than this per step counts as a cut. |
| `asyncStreams` | `1` | Open audio streams off the main thread (fixes the in-race stutter). |
| `fixExitCrash` | `1` | Fix the game's crash on exit (debug font freed twice). |
| `dopplerHoldOnSwitch` | `0.5` | Seconds to hold the Doppler pitch after a camera switch (fixes the game's engine-sound scratching). `0` = original. |
| `lodDistanceScale` | `2.0` | Cars and traffic switch to lower-detail models this many times farther away. `1` = original. |
| `fixCameraCutAudio` | `2` | Updates the sound listener keeps its position after a camera cut (fixes the game's engine-sound drop-out). `0` = original. |
| `traceCameraAudio` | `0` | Diagnostic: log sounds started/stopped around camera switches. |
| `profileAudio` | `0` | Diagnostic: time audio calls and log what stalls frames. |
| `log` / `statsSeconds` | `1` / `10` | Log file and how often stats are written. |

## Reading the log

Every `statsSeconds`, one line is written, for example:

```
stats 10.0s: steps=300 (30.0/s) frames=1440 (144.0 fps) ... blended=1440 cut=3 ... car draws=... delta=... no-entry=0 ...
```

- `steps/s` should equal `simHz`.
- `blended` should be close to `frames`; `cut` counts frames drawn unblended (camera switches).
- `mismatch` counts objects moved outside a step (drawn unblended).
- `car draws ... no-entry` above 0 means a car's matrix wasn't found among the tracked objects. That car is drawn at step rate (please report).

## How it works

See the header comment in `MC2FrameFix.cpp` for the addresses and the full reverse-engineering notes.

## Building

```
i686-w64-mingw32-g++ -std=c++17 -O2 -shared -s -static-libgcc -static-libstdc++ \
  -o MC2FrameFix.asi MC2FrameFix.cpp -lwinmm
```
