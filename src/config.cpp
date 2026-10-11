#include "config.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cwchar>

#include "log.h"
#include "util.h"

namespace cfg {

Settings S;

namespace {

std::wstring g_path;
bool g_dirty = false;
double g_dirtySince = 0;

bool ReadRaw(const wchar_t* section, const wchar_t* key, wchar_t (&buf)[64]) {
  GetPrivateProfileStringW(section, key, L"", buf, 64, g_path.c_str());
  return buf[0] != 0;
}

float ReadFloat(const wchar_t* section, const wchar_t* key, float def) {
  wchar_t buf[64];
  if (!ReadRaw(section, key, buf)) return def;
  wchar_t* end = nullptr;
  const float v = wcstof(buf, &end);
  return end != buf ? v : def;
}

int ReadInt(const wchar_t* section, const wchar_t* key, int def) {
  wchar_t buf[64];
  if (!ReadRaw(section, key, buf)) return def;
  wchar_t* end = nullptr;
  const long v = wcstol(buf, &end, 0);  // accepts decimal or 0x hex
  return end != buf ? static_cast<int>(v) : def;
}

// Clamps a setting into range; NaN or infinity (e.g. "nan" typed into the ini) falls back to |def|.
float Sanitized(float v, float lo, float hi, float def) { return std::isfinite(v) ? util::Clamp(v, lo, hi) : def; }

}  // namespace

Settings Defaults() { return Settings{}; }

void Load(const std::wstring& path) {
  g_path = path;
  S = Defaults();
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    S.enabled = ReadInt(L"General", L"Enabled", S.enabled) != 0;
    S.startDelay = Sanitized(ReadFloat(L"General", L"StartDelay", S.startDelay), 0.f, kMaxStartDelay, S.startDelay);
    S.popup = ReadInt(L"General", L"ShowPopup", S.popup) != 0;
    S.keyToggle = ReadInt(L"Keys", L"ToggleMod", S.keyToggle) & 0xFF;
    S.verbose = ReadInt(L"Debug", L"Verbose", S.verbose) != 0;
  }
  Save();  // creates the file on first run and adds any options introduced by updates
  LOG("Settings: enabled=%d startDelay=%.2f popup=%d toggleKey=0x%02X verbose=%d", S.enabled, S.startDelay, S.popup,
      S.keyToggle, S.verbose);
}

void Save() {
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_path.c_str(), L"w") != 0 || !f) {
    LOG("Couldn't write %ls", g_path.c_str());
    return;
  }
  fprintf(f,
          "; AutoLockpick settings (Hellraiser: Revival). Edit this file while the game is closed; the hotkey\n"
          "; below switches the mod on and off in game.\n\n[General]\n");
  fprintf(f, "; 1 = lockpicking puzzles are solved for you when you carry a lockpick. 0 = you pick them yourself.\n");
  fprintf(f, "Enabled=%d\n", S.enabled ? 1 : 0);
  fprintf(f, "; Seconds after the close-up of the lock opens before the pick starts to move (0 to %.0f).\n",
          kMaxStartDelay);
  fprintf(f, "StartDelay=%.2f\n", S.startDelay);
  fprintf(f, "; Show a popup when the hotkey switches the mod on or off.\n");
  fprintf(f, "ShowPopup=%d\n", S.popup ? 1 : 0);
  fprintf(f,
          "\n[Keys]\n; Windows virtual-key code, 0 for none: "
          "https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes\n");
  fprintf(f, "ToggleMod=0x%02X\n", S.keyToggle);
  fprintf(f, "\n[Debug]\n; 1 = write every step of each lockpick to AutoLockpick.log.\n");
  fprintf(f, "Verbose=%d\n", S.verbose ? 1 : 0);
  fclose(f);
}

void MarkDirty() {
  g_dirty = true;
  g_dirtySince = util::Now();
}

void SaveIfDirty(double now) {
  if (g_dirty && now - g_dirtySince > 1.0) {
    g_dirty = false;
    Save();
  }
}

}  // namespace cfg
