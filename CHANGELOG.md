# MC2FrameFix changelog

MC2FrameFix is an ASI plugin for **Midnight Club II** (`mc2.exe`, PC). It adds:
- fixed-timestep physics;
- frame interpolation;
- an optional frame limiter.

Every patch checks the original bytes first. On any other exe, the mod leaves the game alone.

---

## 0.3.2: smoother camera switches, instant-switch option

- **No hold at the start and end of the switch swoop.** The game's swoop swaps the view's camera object twice:
  - at the start, to a transition camera that begins as a copy of the old one (0x57C7A0);
  - at the end, back to the new camera where the transition finished.

  Each swap looked like a brand-new camera with no earlier pose, so the camera was held at its step pose for a frame or two at both ends (`cam-anchored`): the little stutter. A new camera object now takes its previous pose from the view's previous camera at the last step, and the step is blended (`cam-handoff`). A swap where the camera jumps more than 2 m or 20° relative to the car is still treated as a cut: look-back, or the instant switch to the bumper camera.
- **`instantCameraSwitch=1` (option, off by default):** the camera button cuts straight to the next camera, as the game already does for the bumper camera, instead of flying there through the car in half a second. Doppler stays on for the other cameras, as after a normal switch. Use it to check whether the remaining audio cut-off comes from the swoop.

## 0.3.1: camera-switch scratching

- **Engine sound no longer warbles on a camera switch (`dopplerHoldOnSwitch`, default 0.5 s).** This is a bug in the original game.
  - The game hands the camera to the sound manager as the listener every update (0x469B60 → 0x5D6050). Each sound's Doppler pitch comes from how fast the listener moved toward or away from it since the last frame (0x5D5CD0).
  - A switch flies the camera through the car to the new view in about 0.3 s, in uneven steps. Against the car's own engine that is tens of m/s, back and forth, so every engine layer's pitch jumped by several percent each frame. That's the scratching that the reAL trace showed as pitch going up and down every frame.
  - The game already turns Doppler off for one camera type. The same flag is now set for `dopplerHoldOnSwitch` seconds after the camera changes, so sounds keep their last Doppler pitch through the swoop. `0` restores the original behaviour.
- **Log:** camera switches and how many listener updates had Doppler held.

## 0.3.0: exit crash fix

- **No more crash on exit (`fixExitCrash=1`).** The game frees its debug font twice at shutdown.
  - The main shutdown (0x53A870 → 0x5F9E30) frees it and clears the pointer.
  - A shutdown callback registered when the font was created (0x5F9D70) then runs and decrements the refcount through the cleared pointer: access violation at `mc2.exe+1F9D76` (null + 0x18).
  - The callback now returns when the font is already gone. Without the fix it would also free the shared font page a second time.
- Camera-switch audio scratch: confirmed to be the original game. On a switch it hard-stops the playing engine-sound layers and starts the new camera's set in the same frame. Nothing changed for it here.

## 0.2.9: camera switch swoop

- **Smooth switch swoop.** When you change camera, the game flies the camera from the old view to the new one in about a third of a second, through the car. It moves so fast that 0.2.8 took most of those steps for cuts and drew the camera at 30 Hz during the swoop: the stutter left after 0.2.8.
- **Cameras are now blended relative to the car they follow.** The camera's pose relative to that car is blended between the two steps and put on the car's blended matrix.
  - The followed car is found as the moving physics object the camera moves least against, within 50 m.
  - The swoop, hood and bumper cameras, and chase cameras through hard turns, stay locked to the car.
  - A camera that moves less in world space than relative to any car (a fixed trackside camera) is blended in world space, as before.
  - Real cuts are anchored to the car as in 0.2.8. A real cut is a new camera, a look-back, or a jump in both spaces.
- **Log:** `cams world` / `car-relative` count camera blends in each space.

## 0.2.8: smooth camera switches

- **Camera switches no longer hitch.** On a switch (or look-back, or any camera jump) the new camera has no earlier pose to blend from, so the whole frame used to be drawn at the newest step. That put the world a fraction of a step ahead, then the same picture was shown for up to three frames until the new camera had two steps: a visible jump and a short freeze.
  - The world now stays blended through a switch. The new camera rides on the blend of the car it follows (the nearest moving physics object): exactly where the step puts it relative to that car. From the next step it blends as usual.
  - With nothing moving nearby (a fixed trackside camera, a car standing still), the camera is drawn at its step pose and the world stays blended.
  - A camera moved outside a step still draws the frame unblended, as before.
- **Streams start sooner.** Only the first 1 MB of a stream is read ahead before opening it, not the whole file; that is all Miles reads during the open. The rest streams in while playing, as in the original game.
- **Log:** `cam-anchored` counts frames with a switched camera carried by its car; `cut` now counts camera cuts shown at the step pose. Each stats period lists up to 8 stream opens with their file name and how long the sound waited (read-ahead and open time).

## 0.2.7: profiler off

- Stream fix confirmed in game: after loading, no frame over 26 ms and no audio call over 1 ms on the main thread.
- `profileAudio` now defaults to `0`. Set it to `1` to time audio calls again. An existing INI keeps its value, so set it to `0` there too.

## 0.2.6: stutter fix (audio streams)

- **The in-race hitches are gone (`asyncStreams=1`).** The 0.2.5 profiler log pinned them on Miles `AIL_open_stream`: the game opens city-ambience and voice streams on the main thread, and a cold open (the file not yet in the OS cache) stalls that frame for ~100 ms. DirectMusic was not the cause; its loads already run on their own thread.
  - Opens now return at once, and a worker thread does the real open. Start, pause and volume/pan calls made meanwhile are kept and applied as soon as the stream exists (usually within a frame or two).
  - Before opening, the worker reads the file once to warm the OS cache, so the part of the open Miles does under its own lock stays short. Loose files are read with plain Windows calls; packed ones through the game's own file callbacks (which Miles already calls from its streaming thread in the original game).
  - If the open fails, the stream reports "done" and the game closes it, as with any finished stream. If the pool of 64 streams is full, the open runs the old way.
- **Log:** the stats line shows streams opened, the longest open, MB prefetched, failed opens and fallbacks to the old path.
- `profileAudio` stays on in this build to confirm nothing on the main thread still waits on Miles.

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
