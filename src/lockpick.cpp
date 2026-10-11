#include "lockpick.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "game.h"
#include "log.h"
#include "ue.h"
#include "util.h"

namespace lockpick {

Status St;

namespace {

using game::C;
using game::F;
using game::O;

constexpr char kPuzzleClass[] = "BP_EnvPuzzleBaseActor_LockpickPuzzle_C";
constexpr char kLockpickItem[] = "Item_Lockpick";
// How fast the pick turns towards the correct angle, in degrees per second of game time.
constexpr double kAimSpeed = 160.0;
// Close enough to stop turning the pick: the game accepts anything within CorrectAngleOffset.
constexpr double kAimedWithin = 0.05;
// Tension held this long (game seconds) without the lock opening means the puzzle didn't respond as
// expected: the mod lets go and leaves the lock to the player.
constexpr double kTurnTimeout = 8.0;
// Tries per lock: the game cancels a success if the tension lapses while the lock is opening.
constexpr int kMaxAttempts = 3;
constexpr int32_t kMaxParams = 256;

const char* PhaseName(Phase phase) {
  switch (phase) {
    case Phase::Idle: return "idle";
    case Phase::Waiting: return "waiting";
    case Phase::Aiming: return "aiming";
    case Phase::Turning: return "turning";
    case Phase::Opened: return "opened";
    case Phase::LeftToPlayer: return "left to the player";
  }
  return "?";
}

// A Blueprint "real" (double) or float property.
double ReadReal(void* base, const ue::Prop& p) {
  return p.size == 8 ? ue::At<double>(base, p.offset) : static_cast<double>(ue::At<float>(base, p.offset));
}

const ue::Param* FindParam(const std::vector<ue::Param>& params, const char* name) {
  for (const ue::Param& p : params) {
    if (!p.isReturn && ue::SameText(p.name, name)) return &p;
  }
  return nullptr;
}

// The |nth| (from 0) non-return parameter of type |type|, or nullptr.
const ue::Param* ParamOfType(const std::vector<ue::Param>& params, const char* type, int nth = 0) {
  for (const ue::Param& p : params) {
    if (!p.isReturn && ue::SameText(p.type, type) && nth-- == 0) return &p;
  }
  return nullptr;
}

std::string Join(const std::vector<std::string>& v) {
  std::string out;
  for (const std::string& s : v) out += (out.empty() ? "" : ", ") + s;
  return out;
}

// ---- the puzzle's reflection data ----------------------------------------------------------

// What the mod reads and calls on a lockpick puzzle, looked up once per Blueprint class.
struct PuzzleClass {
  void* cls = nullptr;
  bool lockpick = false;  // the class is, or derives from, the lockpick puzzle
  bool usable = false;    // ...and has everything below
  ue::Prop active;        // bPuzzleActive
  ue::Prop finished;      // PuzzleFinished: the lock has turned all the way
  ue::Prop failed;        // bPuzzleFailed: the pick broke
  ue::Prop blockLook;     // BlockLookInput: the pick can't move (the lock is under tension)
  ue::Prop unlocking;     // bIsUnlocking: tension applied
  ue::Prop correctAngle;  // CorrectAngle
  ue::Prop angleOffset;   // CorrectAngleOffset: the tolerance (log only)
  ue::Prop pickRotation;  // ParentLockpickRotation
  int32_t requiredItem = -1;  // RequiredItem (an item definition), may be unset
  ue::Function lookInput;     // PuzzleLookInput(FVector2D AxisValue): turns the pick by -X degrees
  ue::Function inputY;        // PuzzleInputY(float AxisValue): tension
  int32_t lookAxis = -1, lookAxisSize = 0, yAxis = -1, yAxisSize = 0;
};

// The puzzle task (BP_TASK_FocusEnvPuzzle) that ACorePlayerState::PuzzleRef points at.
struct TaskClass {
  void* cls = nullptr;
  int32_t puzzleActor = -1;  // PuzzleActorRef
  ue::Prop active;           // bPuzzleActive: cleared when the game finishes the puzzle
};

bool IsLockpickPuzzleClass(void* cls) {
  int depth = 0;
  for (void* s = cls; s && depth < 32; s = ue::SuperOf(s), ++depth) {
    if (ue::NameEquals(ue::At<ue::FName>(s, ue::L.objName), kPuzzleClass)) return true;
  }
  return false;
}

const PuzzleClass& DescribePuzzle(void* cls) {
  static PuzzleClass cache[4];
  static unsigned next = 0;
  for (const PuzzleClass& c : cache) {
    if (c.cls == cls) return c;
  }
  PuzzleClass& c = cache[next++ % 4];
  c = PuzzleClass{};
  c.cls = cls;
  c.lockpick = IsLockpickPuzzleClass(cls);
  if (!c.lockpick) return c;

  std::vector<std::string> missing;
  const auto prop = [&](const char* name, ue::Prop& out, bool real = false) {
    out = ue::FindProp(cls, name);
    if (!out.Valid() || (real && out.size != 8 && out.size != 4)) {
      missing.push_back(name);
      out = ue::Prop{};
    }
  };
  prop("bPuzzleActive", c.active);
  prop("PuzzleFinished", c.finished);
  prop("bPuzzleFailed", c.failed);
  prop("BlockLookInput", c.blockLook);
  prop("bIsUnlocking", c.unlocking);
  prop("CorrectAngle", c.correctAngle, true);
  prop("ParentLockpickRotation", c.pickRotation, true);
  c.angleOffset = ue::FindProp(cls, "CorrectAngleOffset");
  c.requiredItem = ue::Offset(cls, "RequiredItem");

  const auto axis = [&](ue::Function& fn, const char* name, int32_t& offset, int32_t& size, int32_t floatSize) {
    const ue::Param* p = fn.Bind(cls, name) ? FindParam(fn.Params(), "AxisValue") : nullptr;
    if (!p || fn.ParamsSize() > kMaxParams || (p->size != floatSize && p->size != floatSize * 2)) {
      missing.push_back(std::string(name) + "(AxisValue)");
      return;
    }
    offset = p->offset;
    size = p->size;
  };
  axis(c.lookInput, "PuzzleLookInput", c.lookAxis, c.lookAxisSize, 8);  // FVector2D: two floats or two doubles
  axis(c.inputY, "PuzzleInputY", c.yAxis, c.yAxisSize, 4);              // float or double

  c.usable = missing.empty();
  if (c.usable) {
    LOG("Lockpick puzzle: %s (correct angle at +0x%X, pick at +0x%X, tolerance %s, required item %s)",
        ue::FullName(cls).c_str(), c.correctAngle.offset, c.pickRotation.offset,
        c.angleOffset.Valid() ? "found" : "not found", c.requiredItem >= 0 ? "found" : "not found");
  } else {
    LOG("Lockpick puzzle %s: not found: %s. Lockpicking is left to you.", ue::FullName(cls).c_str(),
        Join(missing).c_str());
  }
  if (cfg::S.verbose) ue::DumpStruct(cls);
  return c;
}

const TaskClass& DescribeTask(void* cls) {
  static TaskClass cache[4];
  static unsigned next = 0;
  for (const TaskClass& c : cache) {
    if (c.cls == cls) return c;
  }
  TaskClass& c = cache[next++ % 4];
  c = TaskClass{};
  c.cls = cls;
  const ue::Prop actor = ue::FindProp(cls, "PuzzleActorRef");
  if (actor.Valid() && ue::SameText(actor.type, "ObjectProperty")) c.puzzleActor = actor.offset;
  c.active = ue::FindProp(cls, "bPuzzleActive");
  LOG("Puzzle task: %s (puzzle actor %s, active flag %s)", ue::FullName(cls).c_str(),
      c.puzzleActor >= 0 ? "found" : "NOT FOUND", c.active.Valid() ? "found" : "not found");
  return c;
}

// ---- calls into the game -------------------------------------------------------------------

void Look(void* puzzle, const PuzzleClass& c, double x) {
  alignas(16) uint8_t p[kMaxParams] = {};
  if (c.lookAxisSize == 16) {
    ue::At<ue::FVector2D>(p, c.lookAxis) = {x, 0.0};
  } else {
    ue::At<float>(p, c.lookAxis) = static_cast<float>(x);
  }
  c.lookInput.Call(puzzle, p);
}

void Tension(void* puzzle, const PuzzleClass& c) {
  alignas(16) uint8_t p[kMaxParams] = {};
  if (c.yAxisSize == 8) {
    ue::At<double>(p, c.yAxis) = 1.0;
  } else {
    ue::At<float>(p, c.yAxis) = 1.f;
  }
  c.inputY.Call(puzzle, p);
}

// Game time in seconds (stops while the game is paused), or the wall clock if it can't be read.
double GameTime(void* worldContext) {
  static int32_t context = -2;
  const ue::Function& fn = F.GetTimeSeconds;
  if (context == -2) {
    const std::vector<ue::Param> params = fn.Params();
    const ue::Param* p = ParamOfType(params, "ObjectProperty");
    context = fn.Bound() && p && fn.Return() >= 0 && (fn.ReturnSize() == 8 || fn.ReturnSize() == 4) &&
                      fn.ParamsSize() <= kMaxParams && C.GameplayStaticsCDO
                  ? p->offset
                  : -1;
    if (context < 0) LOG("Game time not available: timing uses the wall clock");
  }
  if (context < 0) return util::Now();
  alignas(16) uint8_t p[kMaxParams] = {};
  ue::At<void*>(p, context) = worldContext;
  fn.Call(C.GameplayStaticsCDO, p);
  return fn.ReturnSize() == 8 ? ue::At<double>(p, fn.Return()) : ue::At<float>(p, fn.Return());
}

// The lockpick item definition (Item_Lockpick), for puzzles that don't name their required item.
void* LockpickItem() {
  static void* cached = nullptr;
  if (cached && ue::IsValid(cached)) return cached;
  cached = nullptr;
  const int32_t n = ue::NumObjects();
  for (int32_t i = 0; i < n; ++i) {
    void* obj = ue::ObjectAt(i);
    if (!obj || ue::IsDefaultObject(obj) || !ue::NameEquals(ue::At<ue::FName>(obj, ue::L.objName), kLockpickItem)) {
      continue;
    }
    if (C.ItemInfoBase ? ue::IsA(obj, C.ItemInfoBase) : !ue::SameText(ue::ClassName(obj), "Package")) {
      cached = obj;
      break;
    }
  }
  return cached;
}

// How many |item|s the player carries, through the game's own inventory (ItemManager); -1 if the
// game can't be asked, in which case the game's own check (no lockpick, no puzzle) is relied on.
int CountCarried(void* worldContext, void* pawn, void* item) {
  struct Layout {
    bool checked = false, ok = false;
    int32_t context = -1, manager = -1, player = -1, item = -1, count = -1;
  };
  static Layout l;
  if (!l.checked) {
    l.checked = true;
    const ue::Function& get = F.GetItemManager;
    const ue::Function& count = F.GetItemCountInInventory;
    const std::vector<ue::Param> gp = get.Params(), cp = count.Params();
    const ue::Param* context = ParamOfType(gp, "ObjectProperty");
    const ue::Param* player = ParamOfType(cp, "ObjectProperty", 0);
    const ue::Param* what = ParamOfType(cp, "ObjectProperty", 1);
    l.ok = get.Bound() && count.Bound() && C.CoreGameplayStaticsCDO && context && player && what &&
           get.Return() >= 0 && get.ReturnSize() == 8 && count.Return() >= 0 && count.ReturnSize() == 4 &&
           get.ParamsSize() <= kMaxParams && count.ParamsSize() <= kMaxParams;
    if (l.ok) {
      l.context = context->offset;
      l.manager = get.Return();
      l.player = player->offset;
      l.item = what->offset;
      l.count = count.Return();
      LOG("Inventory check: ItemManager::GetItemCountInInventory(%s, %s, ...)", player->name.c_str(),
          what->name.c_str());
    } else {
      LOG("Inventory check not available: relying on the game's own lockpick check");
    }
  }
  if (!l.ok || !pawn || !item) return -1;
  alignas(16) uint8_t p[kMaxParams] = {};
  ue::At<void*>(p, l.context) = worldContext;
  F.GetItemManager.Call(C.CoreGameplayStaticsCDO, p);
  void* manager = ue::At<void*>(p, l.manager);
  if (!manager) return -1;
  alignas(16) uint8_t q[kMaxParams] = {};
  ue::At<void*>(q, l.player) = pawn;
  ue::At<void*>(q, l.item) = item;
  F.GetItemCountInInventory.Call(manager, q);
  return ue::At<int32_t>(q, l.count);
}

// ---- the solver ------------------------------------------------------------------------------

struct Run {
  void* puzzle = nullptr;
  Phase phase = Phase::Idle;
  double since = 0;   // game time the phase started
  double last = -1;   // game time last frame
  double opened = 0;  // game time the puzzle opened (its close-up started)
  int attempts = 0;
  bool sawFinished = false;  // the lock turned all the way during this attempt
  bool checkedInventory = false;
};
Run g_run;

void Enter(Phase phase, double t) {
  if (cfg::S.verbose && phase != g_run.phase) {
    LOG("  %s -> %s", PhaseName(g_run.phase), PhaseName(phase));
  }
  g_run.phase = phase;
  g_run.since = t;
  St.phase = phase;
}

// The puzzle closed or the game finished it (the task may let go of the puzzle in the same frame
// the lock opens, so this is also called when the player state stops pointing at it).
void Ended(double t) {
  if (g_run.phase == Phase::Turning && g_run.sawFinished) {
    ++St.opened;
    LOG("Lock opened %.1f s after its close-up opened (%d this session)", t - g_run.opened, St.opened);
    Enter(Phase::Opened, t);
  } else if (g_run.phase == Phase::Aiming || g_run.phase == Phase::Turning) {
    LOG("The lockpick puzzle closed before the lock opened");
  }
}

void Leave() {
  if (g_run.puzzle) {
    Ended(g_run.last);
    if (cfg::S.verbose) LOG("  left the lockpick puzzle");
  }
  g_run = Run{};
  St.phase = Phase::Idle;
}

void Step(void* puzzle, void* task, const TaskClass& tc, const PuzzleClass& c, void* pawn) {
  const double t = GameTime(puzzle);
  const double dt = g_run.last >= 0 ? util::Clamp(t - g_run.last, 0.0, 0.1) : 0.0;
  const bool ticked = g_run.last < 0 || t > g_run.last;  // false while the game is paused
  g_run.last = t;
  if (puzzle != g_run.puzzle) {
    g_run = Run{};
    g_run.puzzle = puzzle;
    g_run.last = t;
    g_run.opened = t;
    Enter(Phase::Waiting, t);
  }

  const bool active = ue::ReadBool(puzzle, c.active) && (!tc.active.Valid() || ue::ReadBool(task, tc.active));
  const bool finished = ue::ReadBool(puzzle, c.finished);

  if (!active) {
    Ended(t);
    if (g_run.phase != Phase::Opened) {
      // Closed (or not open yet): opening it again starts over.
      g_run.attempts = 0;
      g_run.checkedInventory = false;
      Enter(Phase::Waiting, t);
    }
    g_run.opened = t;
    return;
  }
  if (g_run.phase == Phase::Opened && !finished) Enter(Phase::Waiting, t);  // the same lock again

  if (!cfg::S.enabled) {
    if (g_run.phase == Phase::Aiming || g_run.phase == Phase::Turning) LOG("Switched off: the lock is left to you");
    if (g_run.phase != Phase::Opened) Enter(Phase::Waiting, t);
    return;
  }
  if (ue::ReadBool(puzzle, c.failed)) {
    // A pick broke: the player pushed at a wrong angle before the mod took over. The game starts the
    // puzzle again with the next lockpick, if there is one.
    if (g_run.phase != Phase::Waiting && g_run.phase != Phase::LeftToPlayer) Enter(Phase::Waiting, t);
    return;
  }

  switch (g_run.phase) {
    case Phase::Idle:
    case Phase::Opened:
    case Phase::LeftToPlayer: return;

    case Phase::Waiting: {
      if (t - g_run.since < cfg::S.startDelay) return;
      if (ue::ReadBool(puzzle, c.unlocking) || ue::ReadBool(puzzle, c.blockLook)) {
        g_run.since = t - cfg::S.startDelay;  // the player is turning the lock: carry on once they let go
        return;
      }
      if (!g_run.checkedInventory) {
        g_run.checkedInventory = true;
        void* item = c.requiredItem >= 0 ? ue::Ptr(puzzle, c.requiredItem) : nullptr;
        if (!item || !ue::IsValid(item)) item = LockpickItem();
        const int carried = CountCarried(puzzle, pawn, item);
        const double correct = ReadReal(puzzle, c.correctAngle);
        char tolerance[32] = "?";
        if (c.angleOffset.Valid()) snprintf(tolerance, sizeof tolerance, "%.1f", ReadReal(puzzle, c.angleOffset));
        LOG("Lockpick puzzle opened (%s): correct angle %.1f, tolerance %s, lockpicks carried: %d", ue::ObjectName(task).c_str(),
            correct, tolerance, carried);
        if (carried == 0) {
          LOG("No lockpick carried: the lock is left to you");
          Enter(Phase::LeftToPlayer, t);
          return;
        }
      }
      if (++g_run.attempts > kMaxAttempts) {
        LOG("The lock didn't open after %d tries: it's left to you", kMaxAttempts);
        Enter(Phase::LeftToPlayer, t);
        return;
      }
      g_run.sawFinished = false;
      Enter(Phase::Aiming, t);
      [[fallthrough]];
    }

    case Phase::Aiming: {
      if (ue::ReadBool(puzzle, c.blockLook)) return;  // the lock is under tension: the pick can't move now
      const double target = ReadReal(puzzle, c.correctAngle);
      const double diff = target - ReadReal(puzzle, c.pickRotation);
      if (std::fabs(diff) > kAimedWithin) {
        // The puzzle turns the pick by -X: (pick - X) wrapped and clamped to [-90, 90].
        const double step = util::Clamp(diff, -kAimSpeed * dt, kAimSpeed * dt);
        if (step != 0.0) Look(puzzle, c, -step);
        if (std::fabs(target - ReadReal(puzzle, c.pickRotation)) > kAimedWithin) return;
      }
      Look(puzzle, c, 0.0);  // the pick stops: ends its movement sound
      Tension(puzzle, c);
      if (cfg::S.verbose) {
        LOG("  pick at %.2f (correct %.2f): turning the lock", ReadReal(puzzle, c.pickRotation), target);
      }
      Enter(Phase::Turning, t);
      return;
    }

    case Phase::Turning: {
      if (finished) {
        g_run.sawFinished = true;
      } else if (g_run.sawFinished) {
        LOG("The game cancelled the opening lock: trying again");
        Enter(Phase::Waiting, t - cfg::S.startDelay);
        return;
      } else if (t - g_run.since > kTurnTimeout) {
        LOG("The lock didn't open after %.0f s of tension (pick %.2f, correct %.2f): it's left to you", kTurnTimeout,
            ReadReal(puzzle, c.pickRotation), ReadReal(puzzle, c.correctAngle));
        Enter(Phase::LeftToPlayer, t);
        return;
      }
      // Held every frame like a pressed key, until the game finishes the puzzle: letting go while
      // the lock is still opening makes the game turn it back.
      if (ticked) Tension(puzzle, c);
      return;
    }
  }
}

void* LocalController(void* viewportClient) {
  void* gameInstance = ue::Ptr(viewportClient, O.GVC_GameInstance);
  if (!gameInstance) return nullptr;
  const auto& players = ue::At<ue::TArray<void*>>(gameInstance, O.GI_LocalPlayers);
  if (!players.data || players.num <= 0 || players.num > 16 || !players.data[0]) return nullptr;
  return ue::Ptr(players.data[0], O.Player_PC);
}

}  // namespace

void OnFrame(void* viewportClient) {
  void* pc = LocalController(viewportClient);
  void* state = pc ? ue::Ptr(pc, O.Ctrl_PlayerState) : nullptr;
  void* task = state && ue::IsA(state, C.CorePlayerState) ? ue::Ptr(state, O.PS_PuzzleRef) : nullptr;
  if (!task || !ue::IsValid(task)) return Leave();
  const TaskClass& tc = DescribeTask(ue::ClassOf(task));
  void* puzzle = tc.puzzleActor >= 0 ? ue::Ptr(task, tc.puzzleActor) : nullptr;
  if (!puzzle || !ue::IsValid(puzzle)) return Leave();
  const PuzzleClass& c = DescribePuzzle(ue::ClassOf(puzzle));
  if (!c.lockpick || !c.usable) return Leave();  // another kind of puzzle
  Step(puzzle, task, tc, c, ue::Ptr(pc, O.Ctrl_Pawn));
}

}  // namespace lockpick
