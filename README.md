# AutoLockpick - Lockpicking that solves itself in Hellraiser: Revival

A mod for Clive Barker's Hellraiser: Revival: when you carry a lockpick, the lockpicking puzzle solves itself. The close-up of the lock opens as usual, then the pick turns to the right angle and the lock turns open, through the game's own puzzle, so lockpicks are used exactly as they would be by a perfect pick.

Download and install instructions are on Nexus Mods (search for AutoLockpick in the Clive Barker's Hellraiser: Revival section). This repository is the full source.

## Features

- Every lockpick lock: locked doors, lockers and safes all use the game's one lockpick puzzle.
- Only with a lockpick: the mod checks your inventory before it touches a lock (and the game doesn't open the puzzle without one anyway).
- A perfect pick, played through the game's own puzzle: the pick never breaks, a lockpick is spent only on the locks the game always spends one on, you hear the success sound, and it counts towards the lockpicking achievement.
- F7 switches it on and off at any time, with a small popup, for when you want to pick a lock yourself.
- Works alongside TrueFOV (both can be installed).
- Update-friendly: everything is found by name in the game's own reflection data instead of hard-coded addresses.

## Requirements (to play)

None. It's a standalone `dsound.dll`: no UE4SS, ReShade or other loader needed.

## Building

Requires Windows and the Visual Studio 2022 Build Tools (C++ workload). Run `build.bat`; it builds `build\dsound.dll` and `build\offline_check.exe`. To install your build, put `dsound.dll` in `Hellraiser\Binaries\Win64\` in the game folder, next to `Hellraiser-Win64-Shipping.exe`.

`offline_check.exe <path to Hellraiser-Win64-Shipping.exe> build\dsound.dll` maps the game executable without running it and checks the code search that finds the hooked function (also with TrueFOV's hook next to it), the vtable hooking and the dsound forwarding against the installed game. `tools/gen_proxy_exports.py` regenerates `src/proxy_exports.h` from the system `dsound.dll`. `tools/package.ps1` builds, runs the check, generates the readme from the Nexus description (`tools/bbcode_to_readme.py`), writes the release zip and checks it with Windows Defender (`tools/defender.py`).

## How it works

The game loads `dsound.dll` from its own folder first (it imports DirectSound by ordinal). This one forwards every real dsound function, with the same ordinals, to `C:\Windows\System32\dsound.dll` and starts the mod inside the game process. The engine access (object and name tables, reflection, `ProcessEvent`) is TrueFOV's.

- **The game's lockpicking:** every lockpick lock is the Blueprint `BP_EnvPuzzleBaseActor_LockpickPuzzle`, opened by a `BP_TASK_FocusEnvPuzzle` task. The pick turns between -90 and 90 degrees; the lock turns all the way only when the pick is within `CorrectAngleOffset` degrees of `CorrectAngle`, picked at random when the level loads. Elsewhere the lock turns part way, the tension wrench shakes and pressure builds until the pick breaks (a lockpick spent). When the lock turns all the way, the puzzle waits 0.25 s, spends a lockpick if the lock is flagged to (`bShouldSpendLockpickOnFinish`), and finishes its task, which opens the lock.
- **Finding the puzzle:** a task that opens a puzzle stores itself in `ACorePlayerState::PuzzleRef` and clears it when the puzzle closes. Every frame the mod follows the local player's controller to its player state, `PuzzleRef` and the task's `PuzzleActorRef`; when that's a lockpick puzzle that's open (`bPuzzleActive`), it plays it. Nothing is scanned or polled across the object table.
- **Playing it:** after a short delay (the close-up settling in), the mod checks the inventory (`CoreGameplayStatics::GetItemManager`, `ItemManager::GetItemCountInInventory`), turns the pick to `CorrectAngle` through the puzzle's own look input (`PuzzleLookInput`, a few frames like a mouse move, with its sound), then holds the tension (`PuzzleInputY`) every frame like a pressed key until the game finishes the puzzle. Holding through the end matters: the puzzle's release timer would otherwise turn the lock back during its 0.25 s success delay. If you're turning the lock yourself, the mod waits until you let go.
- **The per-frame callback:** `UGameViewportClient::PostRender` only forwards to `DrawTransition`, the next vtable slot (found by its "LoadingMessage" text), so the mod hooks `DrawTransition`: it runs every frame on the game thread with a canvas for the popup, and leaves `PostRender`'s slot to TrueFOV. TrueFOV 1.0.1 and earlier find `PostRender` by walking that vtable up to `DrawTransition` and stop at the first entry outside the game's code, so when a `winmm.dll` mod is in the game folder this mod waits until `PostRender` has been hooked before hooking `DrawTransition`.

There is no network code; the mod writes only its own settings and log files in the game's `Binaries\Win64` folder.

## Antivirus false positives

Some antivirus tools may flag `dsound.dll` because it is an unsigned proxy DLL that hooks into the game. This is a false positive. The full source is here, so you can check it or build the DLL yourself with `build.bat`.

## Privacy policy

This program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it.

## License

MIT, see [LICENSE](LICENSE). Clive Barker's Hellraiser: Revival is published by Saber Interactive; this project isn't affiliated with or endorsed by its developers or publisher.
