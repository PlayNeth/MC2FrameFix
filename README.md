# MC2FrameFix

Frame-rate-independent physics and smooth high frame rates for **Midnight Club II** on PC.

In races, Midnight Club II steps its physics once per frame with the real frame time, so the handling, top speed, burnout boost and crash damage change with your frame rate. MC2FrameFix runs the physics at a fixed 30 Hz, the rate the game was tuned for, and draws frames between physics steps, so the game handles as intended while drawing smoothly at 60, 144 Hz and higher. It also fixes several of the game's stutters, sound glitches and crashes. It's a single file, `MC2FrameFix.asi`, and it doesn't touch the game's files on disk.

---

## Contents

- [Requirements](#requirements)
- [Installing](#installing)
- [Uninstalling](#uninstalling)
- [What it changes](#what-it-changes)
- [Settings](#settings)
- [Files the mod uses](#files-the-mod-uses)
- [Troubleshooting](#troubleshooting)
- [Reading the log](#reading-the-log)
- [Building from source](#building-from-source)
- [License](#license)

---

## Requirements

- **Midnight Club II** for PC, with `mc2.exe` (SHA-1 `7dc19475132af0724139cb1916a44a42d5368bd8`).
- An **ASI loader**: [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), Win32, named `dinput8.dll` or `version.dll`.
- **To go above 60 fps,** something that lifts the game's v-sync cap: dgVoodoo2, DXVK, a forced refresh rate (3D Analyze), or your graphics driver's settings.

It runs on Windows, and on Linux through Wine or Proton.

## Installing

1. Copy the ASI loader's DLL into the game's folder, the one with `mc2.exe` in it.
2. Copy **`MC2FrameFix.asi`** and **`MC2FrameFix.ini`** next to `mc2.exe`, or into the loader's `scripts` folder.
3. Start a race.

If `MC2FrameFix.ini` is missing, the mod writes it on the first start, with every setting explained. To check it's working, set `log=1` in the INI: `MC2FrameFix.log` then starts with the mod's version and lists every patch it installed.

## Uninstalling

Delete `MC2FrameFix.asi` and `MC2FrameFix.ini`, and the ASI loader's DLL if nothing else uses it. You can also delete `MC2FrameFix.log`.

---

## What it changes

Almost everything can be switched off or adjusted in `MC2FrameFix.ini`. Menus, movies and loading run exactly as in the original game.

### Physics and frame rate

- **Fixed-timestep physics:** the simulation runs in fixed steps (`simHz`, 30 Hz by default) whatever the frame rate, so the handling, top speed, burnout boost and crash damage are the same at any fps.
- **Input every step:** controls are read once per physics step, so a quick tap on a frame without a step isn't lost.
- **Frame limiter** (optional), for playing with v-sync off.

### Smooth motion

- **Everything moving is drawn between physics steps:**
  - your car, opponents and every other active physics object, knocked props included;
  - ambient traffic;
  - pedestrians, with their arms and legs, including a pedestrian you hit;
  - the camera, including its field of view.
- **Whole cars move together:** wheels, shadows and other parts, and the head and tail light glows, follow the blended car.
- **Camera cuts:** camera switches, look-back and big jumps are never blended through. The new camera follows the blended car from its first frame, so a switch is as smooth as the rest.
- **Animations drawn at the right speed:** the HUD, pause menu and other animations the game advances while drawing use the real frame time, as in the original.

### Graphics

- **Detailed models farther away:** trees, props, cars and traffic keep their detailed models twice as far (`lodDistanceScale`).
- **Traffic, pedestrians and props drawn farther:** to 300 m instead of the game's 150 (`ambientDistance`).

### Sound

- **No stutter when sounds start:** city ambience and voice streams are opened in the background. The game opened them while drawing, stalling a frame for about 100 ms each time.
- **No engine scratching on camera switches:** the game's Doppler effect reacted to the camera flying through on every switch. It's now held for half a second.
- **No engine drop-out on camera cuts:** the game briefly put the listener where the new camera was last used. It now waits for the camera's real position.

### Crash fixes

- **No crash at the menu after the intro:** when DirectMusic fails to create one of the game's music channels (seen with Wine's DirectMusic or a wrapper), the game crashed the first time it changed the music volume. The music already played through the default channel; volume changes and fades now go there too.
- **No crash on exit:** the game freed its debug font twice when quitting.

---

## Settings

**`MC2FrameFix.ini`** is next to the `.asi`. Every setting has a note above it saying what it does.

| Setting | Default | What it does |
|---|---|---|
| `enabled` | `1` | `0` runs the game as the original. |
| `simHz` | `30` | Physics rate, 25–1000. `30` is the console tuning; `60` is the original PC feel on a 60 Hz screen. |
| `maxStepsPerFrame` | `8` | The most physics steps per frame, if the PC can't keep up. |
| `fpsLimit` | `0` | Frame limiter, for use with v-sync **off**: set it to your refresh rate. With v-sync on, leave it at `0`. |
| `interpolate` | `1` | Draws between physics steps. |
| `interpolateCamera` | `1` | Blends the camera. |
| `interpolateModels` | `1` | Moves car and traffic parts (wheels, shadow) with the blended body. |
| `interpolateTraffic` | `1` | Ambient traffic. |
| `interpolatePhysics` | `1` | Every active physics object (cars, opponents, props). |
| `interpolatePeds` | `1` | Pedestrians. |
| `interpolateGlows` | `1` | Light glows follow the blended car. |
| `maxStepDistance` | `12` | A step longer than this (metres) counts as a teleport and isn't blended. |
| `maxStepAngle` | `60` | The same for rotation (degrees per step). |
| `maxCameraAngle` | `45` | A camera turning more than this per step counts as a cut. |
| `asyncStreams` | `1` | Opens audio streams in the background (fixes the in-race stutter). |
| `fixExitCrash` | `1` | Fixes the crash on exit. |
| `fixMusicCrash` | `1` | Fixes the crash at the menu after the intro. |
| `dopplerHoldOnSwitch` | `0.5` | Seconds to hold the Doppler pitch after a camera switch. `0` is the original. |
| `fixCameraCutAudio` | `2` | Physics steps the sound listener keeps its position after a camera cut. `0` is the original. |
| `lodDistanceScale` | `2.0` | How many times farther detailed models are kept. `1` is the original. |
| `ambientDistance` | `300` | How far (m) traffic, pedestrians and props are still drawn. The game's is 150. |
| `log` / `statsSeconds` | `0` / `10` | `1` writes `MC2FrameFix.log`, with a stats line every `statsSeconds`. |
| `traceCameraAudio`, `profileAudio` | `0` | Diagnostics: sounds around camera switches, and audio calls that stall frames. |

[CHANGELOG.md](CHANGELOG.md) has the version history.

## Files the mod uses

All next to the `.asi`:

| File | |
|---|---|
| `MC2FrameFix.ini` | Settings. |
| `MC2FrameFix.log` | Only with `log=1`: what the mod did, stats, and where a crash happened. Send it with any bug report. |

## Troubleshooting

- **Nothing changed:** set `log=1`. If no `MC2FrameFix.log` appears, the ASI loader isn't loading; check that its DLL is named `dinput8.dll` or `version.dll` and sits next to `mc2.exe`.
- **The log says `mc2.exe code doesn't match`:** your `mc2.exe` isn't the version the mod was made for. Check it against the SHA-1 above. Nothing is patched, to be safe.
- **Stuck at 60 fps:** the game's v-sync cap is still on. See [Requirements](#requirements).
- **The game feels different from what you're used to:** the mod runs the physics at the console's 30 Hz. `simHz=60` gives the original PC feel on a 60 Hz screen.
- **No music under Wine:** the game's music uses DirectMusic. [reAL](https://github.com/PlayNeth/reAL) with `music=true` plays it. Keep `fixMusicCrash=1` in any case.
- **The game crashes:** set `log=1`. The log records where the crash happened, the registers and the call stack. Please report it with the log.

## Reading the log

With `log=1`, one stats line is written every `statsSeconds`, for example:

```
stats 10.0s: steps=300 (30.0/s) frames=1440 (144.0 fps) ... blended=1440 cut=3 ... car draws=... delta=... no-entry=0 ...
```

- `steps/s` should equal `simHz`.
- `blended` should be close to `frames`; `cut` counts frames drawn unblended (camera switches).
- `mismatch` counts objects moved outside a step, which are drawn unblended.
- `car draws ... no-entry` above 0 means a car wasn't found among the tracked objects, so it's drawn at the physics rate. Please report it.

## Building from source

The source is a single file, `MC2FrameFix.cpp`, built with MinGW-w64, 32-bit:

```
i686-w64-mingw32-g++ -std=c++17 -O2 -shared -s -static-libgcc -static-libstdc++ \
  -o MC2FrameFix.asi MC2FrameFix.cpp -lwinmm
```

The header comment in `MC2FrameFix.cpp` has the game addresses and the full reverse-engineering notes.

## License

GPL-3.0. See [LICENSE](LICENSE). Midnight Club II is © Rockstar Games; this mod contains no game files.
