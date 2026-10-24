# MC2FrameFix changelog

MC2FrameFix is an ASI plugin for **Midnight Club II** (`mc2.exe`, PC). It adds:
- fixed-timestep physics;
- frame interpolation;
- an optional frame limiter.

Every patch checks the original bytes first. On any other exe, the mod leaves the game alone.

---

## 0.2.5: audio profiler (diagnostic)

- **`profileAudio=1`** (on in this build): times every call the game makes into Miles (all `mss32` imports) and into DirectMusic, so the log shows what stalls the main thread.
  - DirectMusic covers the Performance8 (50 methods), Loader8 (12) and Segment8 (28) interfaces of the music and city-ambience player (dxSong).
  - Timing is signature-agnostic: the return address is swapped through a per-thread shadow stack. Return values, including x87 ones, are untouched.
- **Log:**
  - every audio call of 8 ms or more, and which thread made it;
  - each hitch frame over 40 ms, split into update, draw and present/other time, with its audio time and longest audio call;
  - every stats period, the audio functions with the most time, on the main thread and on other threads.

## 0.2.4: draw-time animation speed

- **HUD, pause menu and GO sign at the right speed.** Draw-time code advances animations by `Seconds`. In the original game that's the frame time. With fixed steps it was the step size, while frames are drawn more often than that, so those animations ran 2× fast at 60 fps with `simHz=30`. The GO sign, the pause-menu blinking and other draw-time effects were affected. During Draw, `Seconds` is now the real frame time, with slow motion applied and clamped like the game's own timer. The step value is put back afterwards.
- **`menuHz` removed.** Its premise (menus counting frames) was wrong: their animations are time-based. Throttling the front-end update doubled the time step each frame saw, so it made the main menu faster, not slower. The front end and the pause menu run exactly as in the original again. An existing `menuHz` line in the INI is ignored.
- The Draw hook is installed with fixed steps even when `interpolate=0`.

## 0.2.3: ped skeletons

- **Peds no longer stutter.** A ped is drawn from a world-space bone palette that the game double-buffers. The skeleton update (0x5DE460) writes the pose into the drawn buffer, and the draw swaps it away afterwards (0x541BE0). On a frame without a physics step, the swapped-in older palette was drawn, so the ped's body jumped back and forth every other frame. Moving the mesh by the root's blend couldn't fix that.
  - The newest pose is now copied after every step.
  - Each ped is drawn from a palette blended bone by bone between the last two steps (position lerp, rotation slerp).
  - The limbs animate smoothly too.
  - The game's buffers are restored exactly as its own swap leaves them.

## 0.2.2: all car lights, menu speed, pacing log

- **All car and traffic lights follow the blended car.** The brake flares and other light sprites came from five more light functions (0x51C6F0, 0x51C8A0, 0x51F1F0, 0x51CFF0, 0x51EF00) that 0.2.1 didn't cover. They drew at the newest step, ahead of the blended car, on every blended frame. All seven light functions now get the blended matrix. A matrix that's already blended isn't moved again when one light function calls another.
- **Menus at console speed** (`menuHz`, default 30): the front end and the in-race pause menu count frames for their blinking and animations, so above 30 fps they ran too fast. Their update now runs at `menuHz` while frames are still drawn every frame. `menuHz=0` restores per-frame updates.
- **Peds can turn on the spot:** the rotation limit no longer applies to peds, so an about-face is blended instead of snapping.
- **Log:** a frame-interval histogram and a steps-per-frame histogram in every stats period.

## 0.2.1: peds, light glows, frame pacing

- **Pedestrians interpolated** (`interpolatePeds`): each active ped's root transform is blended. While a ped draws, its skinned mesh moves with the blend. Walk-cycle poses still update per tick.
- **Light glows follow the car** (`interpolateGlows`): tail and head light glows are queued as world-space sprites from the car's matrix (0x51CEA0, 0x51CB00). During a blended car or traffic draw they now get the blended matrix, so they no longer drift off the lamps.
- **Even frame pacing:** the accumulator advances by display time, not raw timer readings.
  - With `fpsLimit`, an on-time frame counts as exactly one period. A late frame counts as its real time.
  - Without `fpsLimit`, frame times are smoothed as in HP2FrameFix.
  - The blend position no longer jitters, and a step can't slip into the next frame at fps = 2× `simHz`.
- **INI note:** use `fpsLimit` only with vsync off. Together they fight, and the result is a skipped refresh (stutter).
- **Log:**
  - Frame time min/max and a count of slow frames.
  - The first 8 objects per stats period that jump too far to blend: kind, distance, turn, position.
  - Ped and glow counters.

## 0.2.0: interpolation

Untested in game. This version was built from disassembly only.

- **Interpolation** (`interpolate`, `interpolateCamera`, `interpolateModels`, `interpolateTraffic`, `interpolatePhysics`):
  - After every fixed step, the world matrices of all active physics colliders, ambient traffic and the active camera of each view are recorded.
  - Before Draw they are written blended between the last two steps, and restored right after Draw. The simulation never sees blended values.
- **Whole cars:** while a car model or a traffic vehicle draws, every world matrix it sets is moved by the body's blend delta. Wheels, shadow and cached parts stay attached.
- **Camera cuts:** a camera switch, a jump, a turn of more than `maxCameraAngle`, or an FOV jump draws the frame unblended.
- **Input per tick:** pre-update (input poll, frame begin) and post-update now run once per simulation tick. They no longer run on frames without a tick.
- **Default `simHz` is now 30** (the game's tuning). The range is 25–1000, because the race timer clamps a step to 0.04 s.
- **Renamed** from `mc2_fixedtick.asi` to `MC2FrameFix.asi`. The INI is created on first start.

## 0.1.0: fixed timestep

- In races, Game::Update runs in fixed steps of `1/SimHz` from an accumulator, with `Seconds` forced to the step size. Slow motion is respected.
- Optional frame limiter.
