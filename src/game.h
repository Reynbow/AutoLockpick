#pragma once

// Everything the mod uses from the game's engine and native code, looked up by name through
// reflection at startup: property offsets, functions it calls, and the vtable slot it hooks. -1 /
// nullptr means "not found" for optional entries; Resolve() fails if a required one is missing.
// The lockpick puzzle itself is a Blueprint class that loads with the levels: lockpick.cpp looks it
// up when it first meets one.

#include <cstdint>
#include <string>

#include "ue.h"

namespace game {

struct Offsets {
  int32_t Eng_GameViewport = -1;       // UEngine::GameViewport
  int32_t Eng_SmallFont = -1;          // UEngine::SmallFont
  int32_t Eng_MediumFont = -1;         // UEngine::MediumFont
  int32_t Eng_LargeFont = -1;          // UEngine::LargeFont
  int32_t Eng_SubtitleFont = -1;       // UEngine::SubtitleFont
  int32_t GVC_GameInstance = -1;       // UGameViewportClient::GameInstance
  int32_t GI_LocalPlayers = -1;        // UGameInstance::LocalPlayers
  int32_t Player_PC = -1;              // UPlayer::PlayerController
  int32_t Ctrl_Pawn = -1;              // AController::Pawn
  int32_t Ctrl_PlayerState = -1;       // AController::PlayerState
  int32_t PS_PuzzleRef = -1;           // ACorePlayerState::PuzzleRef: the puzzle task the player is in
  int32_t Font_CacheType = -1;         // UFont::FontCacheType (uint8: 0 offline bitmap, 1 runtime)
  int32_t Font_LegacyFontSize = -1;    // UFont::LegacyFontSize (int32 points; runtime fonts rasterize at this)
  int32_t Canvas_SizeX = -1;           // UCanvas::SizeX
  int32_t Canvas_SizeY = -1;           // UCanvas::SizeY
  int32_t Canvas_DefaultTexture = -1;  // UCanvas::DefaultTexture
  // Not reflected: UCanvas::Canvas (FCanvas*) and the FCanvas fields GetDPIScale() reads. The
  // values are Unreal Engine 5.6's; each frame they're used only if what they point at is sane.
  int32_t Canvas_Canvas = 0x2E0;
  int32_t FCanvas_Stereo = 0xA0;
  int32_t FCanvas_DPIScale = 0xB0;
};

struct Classes {
  void* CorePlayerState = nullptr;         // ACorePlayerState (/Script/Project)
  void* ItemInfoBase = nullptr;            // UItemInfoBase (/Script/Project): inventory item definitions
  void* GameplayStaticsCDO = nullptr;      // default objects the static functions below are called on
  void* CoreGameplayStaticsCDO = nullptr;
};

struct Functions {
  ue::Function K2DrawTexture;            // UCanvas::K2_DrawTexture
  ue::Function K2DrawText;               // UCanvas::K2_DrawText
  ue::Function K2TextSize;               // UCanvas::K2_TextSize
  ue::Function GetTimeSeconds;           // UGameplayStatics::GetTimeSeconds (optional: game time)
  ue::Function GetItemManager;           // UCoreGameplayStatics::GetItemManager (optional)
  ue::Function GetItemCountInInventory;  // UItemManager::GetItemCountInInventory (optional)
};

struct Slots {
  int DrawTransition = -1;  // UGameViewportClient::DrawTransition(UCanvas*), which PostRender calls every frame
};

extern Offsets O;
extern Classes C;
extern Functions F;
extern Slots S;

// Looks everything up (after ue::Init). Returns false with |error| if something required is missing.
bool Resolve(std::string& error);

// The slot of DrawTransition in the vtable of UGameViewportClient, -1 if not found (also run by
// the offline check). Entries that another mod has hooked (code outside the game) are skipped.
int FindDrawTransitionSlot(void** vtable);

// The engine (UGameEngine), or nullptr before it exists.
void* Engine();
void* GameViewport();

}  // namespace game
