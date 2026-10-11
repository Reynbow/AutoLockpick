#include "game.h"

#include <windows.h>

#include <algorithm>
#include <vector>

#include "log.h"
#include "scan.h"

namespace game {

Offsets O;
Classes C;
Functions F;
Slots S;

namespace {

// The slot in Unreal Engine 5.6, for the log: a different value means the engine changed.
constexpr int kUsualDrawTransition = 112;

void* g_engine = nullptr;

bool Contains(const std::vector<uintptr_t>& v, uintptr_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

void** VtableOfDefault(void* cls) {
  void* cdo = ue::DefaultObject(cls);
  return cdo ? *static_cast<void***>(cdo) : nullptr;
}

// Code in another module: a vtable entry some other mod has pointed at its own hook.
bool IsForeignCode(uintptr_t p) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
  const DWORD access = mbi.Protect & 0xFF;
  return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE ||
         access == PAGE_EXECUTE_WRITECOPY;
}

}  // namespace

// UGameViewportClient::DrawTransition draws the "LOADING" / "PAUSED" messages and is found by
// those strings. PostRender, the slot before it, does nothing but call it, so a hook here runs
// every frame without touching PostRender's slot (TrueFOV hooks that one).
int FindDrawTransitionSlot(void** vtable) {
  if (!vtable) return -1;
  const std::vector<uintptr_t> drawTransition = scan::FunctionsReferencing(L"LoadingMessage");
  for (int slot = 1; slot < 512; ++slot) {
    if (!scan::Readable(reinterpret_cast<uintptr_t>(vtable + slot), sizeof(void*))) break;
    const auto fn = reinterpret_cast<uintptr_t>(vtable[slot]);
    if (!scan::InText(fn)) {
      if (IsForeignCode(fn)) continue;  // another mod's hook
      break;                            // the end of the vtable
    }
    if (!Contains(drawTransition, fn)) continue;
    const bool forwarded = scan::VirtualThunkSlot(reinterpret_cast<uintptr_t>(vtable[slot - 1])) == slot;
    const bool hookedBefore = !scan::InText(reinterpret_cast<uintptr_t>(vtable[slot - 1]));
    LOG("DrawTransition: vtable slot %d (exe+0x%llx; slot %d %s)%s", slot,
        static_cast<unsigned long long>(fn - scan::Img().base), slot - 1,
        forwarded      ? "is PostRender, which calls it"
        : hookedBefore ? "is hooked by another mod (TrueFOV's PostRender hook?)"
                       : "doesn't look like the usual one-line PostRender",
        slot == kUsualDrawTransition ? "" : " - not the usual slot");
    return slot;
  }
  LOG("DrawTransition: not found in the viewport vtable (%zu candidates)", drawTransition.size());
  return -1;
}

bool Resolve(std::string& error) {
  std::vector<std::string> missing;
  const auto need = [&](void* p, const char* what) {
    if (!p) missing.push_back(what);
    return p;
  };
  const auto offset = [&](void* s, const char* sname, const char* prop, int32_t& out, bool optional = false) {
    out = s ? ue::Offset(s, prop) : -1;
    if (out >= 0) return;
    const std::string what = std::string(sname) + "::" + prop;
    if (optional) {
      LOG("Optional %s not found", what.c_str());
    } else {
      missing.push_back(what);
    }
  };
  const auto bind = [&](ue::Function& fn, void* cls, const char* cname, const char* name) {
    if (!cls || !fn.Bind(cls, name)) LOG("Optional %s::%s not found", cname, name);
  };

  void* engine = need(ue::FindClass("Engine"), "UEngine");
  void* viewport = need(ue::FindClass("GameViewportClient"), "UGameViewportClient");
  void* gameInstance = need(ue::FindClass("GameInstance"), "UGameInstance");
  void* player = need(ue::FindClass("Player"), "UPlayer");
  void* controller = need(ue::FindClass("Controller"), "AController");
  void* font = ue::FindClass("Font");
  void* canvas = ue::FindClass("Canvas");
  void* gameplayStatics = ue::FindClass("GameplayStatics");
  C.CorePlayerState = ue::FindObjectIn("Class", "CorePlayerState", "/Script/Project");
  C.ItemInfoBase = ue::FindObjectIn("Class", "ItemInfoBase", "/Script/Project");
  void* coreGameplayStatics = ue::FindObjectIn("Class", "CoreGameplayStatics", "/Script/Project");
  void* itemManager = ue::FindObjectIn("Class", "ItemManager", "/Script/Project");
  if (!C.CorePlayerState) missing.push_back("ACorePlayerState (/Script/Project)");

  offset(engine, "UEngine", "GameViewport", O.Eng_GameViewport);
  offset(engine, "UEngine", "SmallFont", O.Eng_SmallFont, true);
  offset(engine, "UEngine", "MediumFont", O.Eng_MediumFont, true);
  offset(engine, "UEngine", "LargeFont", O.Eng_LargeFont, true);
  offset(engine, "UEngine", "SubtitleFont", O.Eng_SubtitleFont, true);
  offset(viewport, "UGameViewportClient", "GameInstance", O.GVC_GameInstance);
  offset(gameInstance, "UGameInstance", "LocalPlayers", O.GI_LocalPlayers);
  offset(player, "UPlayer", "PlayerController", O.Player_PC);
  offset(controller, "AController", "Pawn", O.Ctrl_Pawn);
  offset(controller, "AController", "PlayerState", O.Ctrl_PlayerState);
  offset(C.CorePlayerState, "ACorePlayerState", "PuzzleRef", O.PS_PuzzleRef);
  offset(font, "UFont", "FontCacheType", O.Font_CacheType, true);
  offset(font, "UFont", "LegacyFontSize", O.Font_LegacyFontSize, true);
  offset(canvas, "UCanvas", "SizeX", O.Canvas_SizeX, true);
  offset(canvas, "UCanvas", "SizeY", O.Canvas_SizeY, true);
  offset(canvas, "UCanvas", "DefaultTexture", O.Canvas_DefaultTexture, true);

  // The popup is drawn with these; without them the mod still works, silently.
  bind(F.K2DrawTexture, canvas, "UCanvas", "K2_DrawTexture");
  bind(F.K2DrawText, canvas, "UCanvas", "K2_DrawText");
  bind(F.K2TextSize, canvas, "UCanvas", "K2_TextSize");
  // Game time (pauses with the game); without it the wall clock is used.
  bind(F.GetTimeSeconds, gameplayStatics, "UGameplayStatics", "GetTimeSeconds");
  // Whether the player carries a lockpick; without these the game's own check is relied on.
  bind(F.GetItemManager, coreGameplayStatics, "UCoreGameplayStatics", "GetItemManager");
  bind(F.GetItemCountInInventory, itemManager, "UItemManager", "GetItemCountInInventory");
  C.GameplayStaticsCDO = ue::DefaultObject(gameplayStatics);
  C.CoreGameplayStaticsCDO = ue::DefaultObject(coreGameplayStatics);

  if (!missing.empty()) {
    error = "not found: ";
    for (size_t i = 0; i < missing.size(); ++i) error += (i ? ", " : "") + missing[i];
    return false;
  }
  // Read as a plain object pointer: anything else (a weak pointer...) would be misread.
  const ue::Prop puzzleRef = ue::FindProp(C.CorePlayerState, "PuzzleRef");
  if (!ue::SameText(puzzleRef.type, "ObjectProperty")) {
    error = "ACorePlayerState::PuzzleRef is a " + puzzleRef.type + ", not an object reference";
    return false;
  }

  S.DrawTransition = FindDrawTransitionSlot(VtableOfDefault(viewport));
  if (S.DrawTransition < 0) {
    error = "UGameViewportClient::DrawTransition not found";
    return false;
  }
  return true;
}

void* Engine() {
  if (!g_engine) {
    void* cls = ue::FindClass("GameEngine");
    g_engine = cls ? ue::FindInstance(cls) : nullptr;
  }
  return g_engine;
}

void* GameViewport() {
  void* engine = Engine();
  return engine ? ue::Ptr(engine, O.Eng_GameViewport) : nullptr;
}

}  // namespace game
