// Offline self-test: maps the game exe into this process (without running it) and checks that the
// code heuristics find the vtable slot the mod hooks (also when TrueFOV has hooked the one before
// it), that vtable hooking works, and that the dsound proxy forwards to the real DLL.
//
// Usage: offline_check.exe <path to Hellraiser-Win64-Shipping.exe> <path to built dsound.dll>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/game.h"
#include "../src/log.h"
#include "../src/scan.h"
#include "../src/ue.h"

namespace {

constexpr int kDrawTransition = 112;  // Unreal Engine 5.6
constexpr int kProcessEvent = 76;

int g_failures = 0;

void Check(bool ok, const char* what) {
  printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
  if (!ok) ++g_failures;
}

void ExpectSlot(const char* name, int got, int want) {
  char buf[160];
  snprintf(buf, sizeof buf, "%-48s = %d (expected %d)", name, got, want);
  Check(got == want, buf);
}

void __fastcall DummyHook() {}

// The start of the vtable holding |slotAddr|: the entries before it are code pointers too. (The
// running mod gets its vtables from live objects; this is only for the offline check.)
void** VtableStart(uintptr_t slotAddr) {
  uintptr_t a = slotAddr;
  while (scan::InText(*reinterpret_cast<const uintptr_t*>(a - 8))) a -= 8;
  return reinterpret_cast<void**>(a);
}

// Vtables in the read-only data that contain the function |fn|.
std::vector<void**> VtablesWith(uintptr_t fn) {
  std::vector<void**> out;
  const scan::Range& r = scan::Img().rdata;
  for (uintptr_t p = r.begin; p + 8 <= r.end; p += 8) {
    if (*reinterpret_cast<const uintptr_t*>(p) != fn) continue;
    void** vt = VtableStart(p);
    if (std::find(out.begin(), out.end(), vt) == out.end()) out.push_back(vt);
  }
  return out;
}

unsigned long long Rva(uintptr_t p) { return static_cast<unsigned long long>(p - scan::Img().base); }

int g_devices = 0;
BOOL CALLBACK CountDevice(LPGUID, LPCWSTR, LPCWSTR, LPVOID) {
  ++g_devices;
  return TRUE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    printf("usage: offline_check <Hellraiser-Win64-Shipping.exe> <dsound.dll>\n");
    return 2;
  }
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  logx::Open((std::wstring(temp) + L"AutoLockpick_offline_check.log").c_str());

  HMODULE game = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!game) {
    printf("couldn't map %ls (error %lu)\n", argv[1], GetLastError());
    return 1;
  }
  scan::Image image;
  if (!scan::Describe(game, image)) {
    printf("not a 64-bit PE image\n");
    return 1;
  }
  scan::SetImage(image);
  printf("Mapped game at %p, image size 0x%x, timestamp %08X\n", static_cast<void*>(game), image.size, image.timeStamp);

  printf("\nUGameViewportClient::DrawTransition:\n");
  const ULONGLONG t0 = GetTickCount64();
  const std::vector<uintptr_t> drawTransition = scan::FunctionsReferencing(L"LoadingMessage");
  printf("  string search took %llu ms\n", GetTickCount64() - t0);
  Check(drawTransition.size() == 1, "one function draws the \"LoadingMessage\" text (DrawTransition)");
  void** viewportVtable = nullptr;
  if (!drawTransition.empty()) {
    printf("  DrawTransition at RVA 0x%llx\n", Rva(drawTransition[0]));
    const std::vector<void**> vtables = VtablesWith(drawTransition[0]);
    Check(!vtables.empty(), "found vtables containing DrawTransition");
    for (void** vt : vtables) {
      char name[80];
      snprintf(name, sizeof name, "DrawTransition slot (vtable RVA 0x%llx)", Rva(reinterpret_cast<uintptr_t>(vt)));
      ExpectSlot(name, game::FindDrawTransitionSlot(vt), kDrawTransition);
      if (!viewportVtable) viewportVtable = vt;
    }
  }
  if (viewportVtable) {
    Check(scan::VirtualThunkSlot(reinterpret_cast<uintptr_t>(viewportVtable[kDrawTransition - 1])) == kDrawTransition,
          "PostRender (the slot before) only forwards to DrawTransition, so it runs every frame");
    // TrueFOV hooks PostRender: the slot before then points at code in another module.
    std::vector<void*> copy(viewportVtable, viewportVtable + kDrawTransition + 32);
    copy.push_back(nullptr);  // where the copied vtable ends
    copy[kDrawTransition - 1] = reinterpret_cast<void*>(&DummyHook);
    ExpectSlot("DrawTransition slot with PostRender hooked", game::FindDrawTransitionSlot(copy.data()), kDrawTransition);
  }

  printf("\nUObject::ProcessEvent:\n");
  ExpectSlot("ProcessEvent slot (viewport vtable)", ue::FindProcessEventSlot(viewportVtable), kProcessEvent);

  printf("\nName lookups (on a hand-made name table):\n");
  {
    // Entries: a 16-bit header (bit 0 wide, bits 6-15 length) and the characters, 2-byte aligned.
    alignas(8) static uint8_t block[96] = {};
    const auto put = [](uint8_t* at, const char* s, bool wide) {
      const auto n = static_cast<uint16_t>(strlen(s));
      const uint16_t header = static_cast<uint16_t>(n << 6 | (wide ? 1 : 0));
      memcpy(at, &header, 2);
      for (uint16_t i = 0; i < n; ++i) {
        if (wide) {
          const wchar_t c = static_cast<unsigned char>(s[i]);
          memcpy(at + 2 + i * 2, &c, 2);
        } else {
          at[2 + i] = static_cast<uint8_t>(s[i]);
        }
      }
    };
    put(block, "bp_envpuzzlebaseactor_lockpickpuzzle_c", false);  // entry id 0
    put(block + 48, "ITEM_LOCKPICK", true);                       // entry id 24 (byte offset 48 / 2)
    static uint8_t* blocks[1] = {block};
    ue::UseNameBlocksForTest(blocks);
    Check(ue::NameEquals({0, 0}, "BP_EnvPuzzleBaseActor_LockpickPuzzle_C"), "the puzzle class name matches in any case");
    Check(ue::NameEquals({24, 0}, "Item_Lockpick"), "\"Item_Lockpick\" matches a wide \"ITEM_LOCKPICK\" entry");
    Check(!ue::NameEquals({24, 0}, "Item_Lockpic") && !ue::NameEquals({24, 0}, "Item_Lockpicks"), "other names don't match");
    ue::UseNameBlocksForTest(nullptr);
  }

  printf("\nVtable hooking (on this process's private copy of the image):\n");
  if (viewportVtable) {
    void* fakeObject = static_cast<void*>(viewportVtable);  // an "object" whose vptr is the vtable
    void* const before = viewportVtable[kDrawTransition];
    void* original = nullptr;
    Check(ue::HookSlot(&fakeObject, kDrawTransition, reinterpret_cast<void*>(&DummyHook), &original),
          "HookSlot wrote the slot");
    Check(original == before, "HookSlot returned the previous function");
    Check(viewportVtable[kDrawTransition] == reinterpret_cast<void*>(&DummyHook), "slot 112 now points at the hook");
  } else {
    Check(false, "no viewport vtable to hook");
  }

  printf("\ndsound proxy:\n");
  HMODULE proxy = LoadLibraryW(argv[2]);
  Check(proxy != nullptr, "proxy DLL loads");
  if (proxy) {
    using Enumerate = HRESULT(WINAPI*)(BOOL(CALLBACK*)(LPGUID, LPCWSTR, LPCWSTR, LPVOID), LPVOID);
    auto enumerate = reinterpret_cast<Enumerate>(GetProcAddress(proxy, "DirectSoundEnumerateW"));
    Check(enumerate != nullptr, "DirectSoundEnumerateW export resolves");
    HMODULE real = GetModuleHandleW(L"C:\\Windows\\System32\\dsound.dll");
    Check(real != nullptr && real != proxy, "forwarder loaded the real System32 dsound.dll");
    if (enumerate) {
      const HRESULT hr = enumerate(&CountDevice, nullptr);
      char buf[96];
      snprintf(buf, sizeof buf, "DirectSoundEnumerateW works through the proxy (0x%08lX, %d devices)",
               static_cast<unsigned long>(hr), g_devices);
      Check(hr == 0, buf);
    }
    for (const char* name : {"DirectSoundCreate", "DirectSoundCreate8", "DirectSoundCaptureCreate8", "GetDeviceID"}) {
      char buf[96];
      snprintf(buf, sizeof buf, "%s export resolves", name);
      Check(GetProcAddress(proxy, name) != nullptr, buf);
    }
  }

  printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
