MC2 FixedTick (.asi) - Midnight Club II PC framerate-independent physics
=========================================================================

INSTALL
1. Get Ultimate ASI Loader (ThirteenAG, GitHub) - the Win32 build.
2. Put its dll in the Midnight Club 2 folder, named  dinput8.dll
   (version.dll also works).
3. Copy mc2_fixedtick.asi and mc2_fixedtick.ini next to mc2.exe.
4. Start a race, quit, check mc2_fixedtick.log: it should say
   "hooks installed". If it says ERROR, your mc2.exe is a different build
   (this one targets SHA1 7dc19475132af0724139cb1916a44a42d5368bd8) and
   nothing was changed.

To break the game's 60 fps vsync cap you still need something that forces a
higher refresh / disables vsync (3D Analyze "force 100 Hz", dgVoodoo2, DXVK,
driver settings...). The plugin only fixes the timing, not the cap.

WHAT WAS FOUND IN THE EXE
- datTimeManager::Update (0x613FC0) measures the real frame time and writes
  it to the global Seconds (0x6797CC) / InvSeconds (0x6797D4). In a race the
  timer runs in "realtime" mode (set up at 0x402F70): dt = real frame time,
  clamped to 0.0001..0.04 s.
- The car code (vehCarSim, wheels, engine, aero, mcCarSim burnout/nitro) is
  written to scale with Seconds, but every physics step is one rendered
  frame, so the integration step size changes with fps. Tire/suspension
  springs and the drivetrain are explicitly integrated, so handling, top
  speed, burnout boost and crash damage drift as fps changes. There's no
  single "missing *dt" bug to patch; the step size itself is the problem.
- The physics simulator has an unused substep system (0x58FED0 / flag
  0x6C9024) that is never turned on.

WHAT THE PLUGIN DOES
- Hooks the race loop's call to Game::Update (0x4021AB) and the timer call
  inside it (0x401CE1).
- Each frame it adds the real elapsed time to an accumulator and runs the
  game update as many times as needed, with Seconds forced to exactly
  1/SimHz (slow-mo time scale 0x6797F0 is respected).
- Frontend, movies, loading, pause and the -framerate fixed mode are left
  untouched.

LIMITATION
For a given SimHz, physics is identical at any fps. To get exactly 30 Hz
console physics while *displaying* more than 30 fps, the renderer has to
interpolate car/camera/traffic positions between ticks. That isn't in this
version yet.
