#pragma once

#include <string>

namespace cfg {

constexpr float kMaxStartDelay = 5.f;

struct Settings {
  bool enabled = true;
  bool giveLockpick = false;  // walking up to a lockpick lock with no lockpick carried adds one
  float startDelay = 0.4f;  // seconds after the lock view opens before the pick starts to move
  bool popup = true;        // show a popup when the hotkey switches the mod on or off
  int keyToggle = 0x76;     // F7; 0 = no hotkey
  bool verbose = false;     // [Debug] Verbose: the puzzle's reflection data and every step in the log
};

extern Settings S;

Settings Defaults();
void Load(const std::wstring& path);
void Save();
void MarkDirty();
void SaveIfDirty(double now);

}  // namespace cfg
