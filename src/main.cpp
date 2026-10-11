// AutoLockpick for Clive Barker's Hellraiser: Revival: the lockpicking puzzle solves itself while
// you carry a lockpick.
//
// Built as dsound.dll: the game imports dsound, so Windows loads this file from the game's
// Binaries\Win64 folder. Every dsound export forwards to the real system DLL (proxy_exports.h).

#include <windows.h>

#include <cwchar>
#include <string>

#include "canvas.h"
#include "config.h"
#include "game.h"
#include "lockpick.h"
#include "log.h"
#include "popup.h"
#include "proxy_exports.h"
#include "scan.h"
#include "ue.h"
#include "util.h"
#include "version.h"

namespace {

constexpr char kVersion[] = ALP_VERSION_STRING;
// How long to wait for the engine to start up before giving up (it takes a few seconds).
constexpr double kStartupTimeout = 300.0;
// How long a winmm.dll mod (TrueFOV) gets to hook PostRender before this one hooks DrawTransition.
constexpr double kPostRenderHookWait = 120.0;

HMODULE g_self = nullptr;
long g_frameFaults = 0;
bool g_keyWasDown = false;

using DrawTransitionFn = void(__fastcall*)(void* self, void* canvas);
DrawTransitionFn g_originalDrawTransition = nullptr;

bool GameHasFocus() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

// The hotkey is read, not intercepted: the game never binds the F keys.
void HandleToggleKey() {
  const int vk = cfg::S.keyToggle;
  const bool down = vk != 0 && GameHasFocus() && (GetAsyncKeyState(vk) & 0x8000) != 0;
  if (down && !g_keyWasDown) {
    cfg::S.enabled = !cfg::S.enabled;
    cfg::MarkDirty();
    LOG("Switched %s with the hotkey", cfg::S.enabled ? "on" : "off");
    if (cfg::S.popup) {
      popup::Show(cfg::S.enabled ? L"AutoLockpick ON" : L"AutoLockpick OFF",
                  cfg::S.enabled ? L"locks are picked for you" : L"you pick locks yourself");
    }
  }
  g_keyWasDown = down;
}

void Frame(void* viewportClient, void* canvas) {
  const double now = util::Now();
  HandleToggleKey();
  lockpick::OnFrame(viewportClient);
  popup::Draw(canvas, now);
  cfg::SaveIfDirty(now);
}

void SafeFrame(void* viewportClient, void* canvas) {
  __try {
    Frame(viewportClient, canvas);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    canvas::EmergencyRestore();  // put back a borrowed font size
    if (InterlockedIncrement(&g_frameFaults) <= 5) LOG("Recovered from a fault in the frame hook (0x%08lX)", GetExceptionCode());
  }
}

// PostRender calls DrawTransition every frame, on the game thread, with the canvas.
void __fastcall HookDrawTransition(void* self, void* canvas) {
  g_originalDrawTransition(self, canvas);
  if (canvas) SafeFrame(self, canvas);
}

// The engine tables are read while the engine is still filling them in on its own thread, so a
// read can land on an object that is half constructed: retry later instead of crashing.
bool SafeInit(std::string& error) {
  __try {
    return ue::Init(error);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void* SafeEngine() {
  __try {
    return game::Engine();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

bool SafeResolve(std::string& error) {
  __try {
    return game::Resolve(error);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void* SafeGameViewport() {
  __try {
    return game::GameViewport();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM param) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId()) return TRUE;
  wchar_t cls[64] = {};
  GetClassNameW(hwnd, cls, 64);
  if (wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
  *reinterpret_cast<bool*>(param) = true;
  return FALSE;
}

// The engine opens its main window once its core has started up (modules loaded, classes
// registered); nothing is read from the game before then.
bool GameWindowExists() {
  bool found = false;
  EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&found));
  return found;
}

std::wstring ModuleDirectory(HMODULE module) {
  wchar_t path[MAX_PATH * 2] = {};
  GetModuleFileNameW(module, path, MAX_PATH * 2);
  std::wstring dir = path;
  return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

// A DLL with this name loaded from the game's folder: another proxy mod, not the Windows file.
bool ModLoadedAs(const wchar_t* dll) {
  HMODULE module = GetModuleHandleW(dll);
  return module && _wcsicmp(ModuleDirectory(module).c_str(), ModuleDirectory(g_self).c_str()) == 0;
}

// TrueFOV (winmm.dll) finds PostRender by walking the viewport's vtable up to DrawTransition, and
// its walk stops at the first entry that isn't the game's code: hooking DrawTransition first would
// cut it short. With a winmm.dll mod in the game folder, PostRender's slot is left to be hooked
// first (the walk is done by then).
void WaitForPostRenderHook(void* viewport) {
  if (!ModLoadedAs(L"winmm.dll")) return;
  void** const* vtable = static_cast<void** const*>(viewport);
  const int postRender = game::S.DrawTransition - 1;
  const double start = util::Now();
  LOG("winmm.dll in the game folder (TrueFOV?): letting it hook PostRender first");
  while (scan::InText(reinterpret_cast<uintptr_t>((*vtable)[postRender]))) {
    if (util::Now() - start > kPostRenderHookWait) {
      LOG("PostRender still not hooked after %.0f s: going ahead", kPostRenderHookWait);
      return;
    }
    Sleep(100);
  }
  LOG("PostRender hooked after %.1f s", util::Now() - start);
}

DWORD WINAPI InitThread(void*) {
  const std::wstring dir = ModuleDirectory(g_self);
  logx::Open((dir + L"AutoLockpick.log").c_str());  // one log per session: each launch starts it fresh
  LOG("AutoLockpick %s for Hellraiser: Revival", kVersion);
  cfg::Load(dir + L"AutoLockpick.ini");

  scan::Image image;
  if (!scan::Describe(GetModuleHandleW(nullptr), image)) {
    LOG("Can't start: the game executable's layout isn't recognised. The game runs normally without the mod.");
    return 0;
  }
  scan::SetImage(image);
  LOG("Game executable: timestamp %08X, %u MB", image.timeStamp, image.size >> 20);

  const double start = util::Now();
  while (!GameWindowExists()) {
    if (util::Now() - start > kStartupTimeout) {
      LOG("Can't start: the game window never appeared. The game runs normally without the mod.");
      return 0;
    }
    Sleep(250);
  }
  LOG("Game window open after %.1f s", util::Now() - start);
  Sleep(1000);

  // Then the engine's object and name tables, the engine object, and everything the mod uses.
  std::string error, lastError;
  int attempts = 0;
  for (;;) {
    error = "fault while reading the engine tables";
    ++attempts;
    if (SafeInit(error)) break;
    if (error != lastError) {
      LOG("Waiting for the engine (attempt %d): %s", attempts, error.c_str());
      lastError = error;
    }
    if (util::Now() - start > kStartupTimeout) {
      LOG("Can't start: %s. The game runs normally without the mod.", error.c_str());
      return 0;
    }
    Sleep(250);
  }
  while (!SafeEngine()) {
    if (util::Now() - start > kStartupTimeout) {
      LOG("Can't start: the engine object never appeared. The game runs normally without the mod.");
      return 0;
    }
    Sleep(250);
  }
  if (!SafeResolve(error)) {
    LOG("Can't start: %s. The game runs normally without the mod.", error.empty() ? "fault while reading the game" : error.c_str());
    return 0;
  }
  LOG("Engine ready after %.1f s", util::Now() - start);

  // The engine creates its viewport a few seconds into startup; hook its DrawTransition to get a
  // per-frame callback on the game thread with a canvas to draw on.
  for (;;) {
    if (void* viewport = SafeGameViewport()) {
      WaitForPostRenderHook(viewport);
      // HookSlot stores the original before the slot changes, so the hook can call it at once.
      if (ue::HookSlot(viewport, game::S.DrawTransition, reinterpret_cast<void*>(&HookDrawTransition),
                       reinterpret_cast<void**>(&g_originalDrawTransition)) &&
          g_originalDrawTransition) {
        LOG("Ready (viewport %s): lockpicking is %s, %s", ue::ClassName(viewport).c_str(),
            cfg::S.enabled ? "automatic" : "left to you (switched off)",
            cfg::S.keyToggle ? "the hotkey switches it" : "no hotkey");
      } else {
        LOG("Can't start: couldn't hook UGameViewportClient::DrawTransition");
      }
      return 0;
    }
    Sleep(100);
  }
}

bool IsGameProcess() {
  wchar_t path[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
  const wchar_t* name = wcsrchr(path, L'\\');
  return _wcsicmp(name ? name + 1 : path, L"Hellraiser-Win64-Shipping.exe") == 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    g_self = module;
    if (IsGameProcess()) {
      if (HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(thread);
    }
  }
  return TRUE;
}
