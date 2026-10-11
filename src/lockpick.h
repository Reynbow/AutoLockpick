#pragma once

// Auto lockpicking. Locked doors, lockers and safes all use the same Blueprint puzzle,
// BP_EnvPuzzleBaseActor_LockpickPuzzle: a pick that turns between -90 and 90 degrees and a tension
// wrench that turns the lock. The lock only turns all the way when the pick is within a few degrees
// (CorrectAngleOffset) of CorrectAngle, picked at random when the level loads.
//
// While the player is in that puzzle and carries a lockpick, the mod plays it the way a perfect
// player would, through the puzzle's own input functions: it turns the pick to the correct angle
// (PuzzleLookInput), then holds the tension (PuzzleInputY) until the lock has turned. The game's own
// success path does the rest: it spends a lockpick if that lock is meant to, plays the success
// sound, counts towards the lockpicking achievement and opens the lock.

namespace lockpick {

enum class Phase {
  Idle,          // not in a lockpick puzzle
  Waiting,       // in one: waiting for the close-up to settle, or for the player to let go of the lock
  Aiming,        // turning the pick to the correct angle
  Turning,       // holding the tension while the lock turns
  Opened,        // the game opened the lock
  LeftToPlayer,  // not solving this one: no lockpick carried, or the puzzle didn't respond as expected
};

struct Status {
  Phase phase = Phase::Idle;
  int opened = 0;  // locks opened this session
};

extern Status St;

// Called once per frame on the game thread.
void OnFrame(void* viewportClient);

}  // namespace lockpick
