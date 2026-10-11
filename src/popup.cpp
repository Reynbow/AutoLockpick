#include "popup.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "canvas.h"
#include "game.h"
#include "ue.h"
#include "util.h"

namespace popup {
namespace {

using canvas::Rect;
using canvas::Role;
using ue::FColor;
using ue::FLinearColor;

constexpr double kShowSeconds = 1.8;
constexpr double kFadeSeconds = 0.35;

constexpr FLinearColor kPanel{0.008f, 0.007f, 0.007f, 0.95f};
constexpr FLinearColor kAccent{0.80f, 0.07f, 0.06f, 1.f};
constexpr FColor Rgb(uint8_t r, uint8_t g, uint8_t b) { return FColor{b, g, r, 255}; }
constexpr FColor kPrimary = Rgb(238, 238, 238);
constexpr FColor kSecondary = Rgb(185, 185, 185);

wchar_t g_title[64] = L"";
wchar_t g_subtitle[96] = L"";
double g_until = 0;
bool g_pending = false;  // shown, its start time not known yet (it's set on the next frame)

FLinearColor Faded(FLinearColor color, float a) {
  color.A *= a;
  return color;
}

FColor Faded(FColor color, float a) {
  color.A = static_cast<uint8_t>(util::Clamp(a, 0.f, 1.f) * color.A);
  return color;
}

}  // namespace

void Show(const wchar_t* title, const wchar_t* subtitle) {
  wcsncpy_s(g_title, title, _TRUNCATE);
  wcsncpy_s(g_subtitle, subtitle, _TRUNCATE);
  g_pending = true;
}

void Draw(void* canvasObject, double now) {
  if (g_pending) {
    g_pending = false;
    g_until = now + kShowSeconds;
  }
  if (now >= g_until || game::O.Canvas_SizeX < 0 || game::O.Canvas_SizeY < 0) return;
  const float ch = static_cast<float>(ue::At<int32_t>(canvasObject, game::O.Canvas_SizeY));
  if (ch < 64.f) return;
  const float u = ch / 1080.f;  // physical pixels per 1080p design pixel
  canvas::Painter p;
  if (!p.Begin(canvasObject, u)) return;

  const float fade = util::Clamp(static_cast<float>((g_until - now) / kFadeSeconds), 0.f, 1.f);
  const auto D = [u](float dp) { return std::round(dp * u); };
  const float lt = p.LineHeight(Role::Title), ls = p.LineHeight(Role::Subtitle);
  const float titleW = p.TextWidth(g_title, Role::Title), subW = p.TextWidth(g_subtitle, Role::Subtitle);
  const float w = std::round((std::max)((std::max)(titleW, subW) + 2.f * D(24.f), D(240.f)));
  const float h = D(12.f) + lt + D(2.f) + ls + D(14.f);
  // Lower than TrueFOV's popup (9% down), so the two never cover each other.
  const float x = std::round((p.Width() - w) * 0.5f), y = std::round(p.Height() * 0.17f);
  p.Fill(Rect{x, y, w, h}, Faded(kPanel, fade));
  p.Fill(Rect{x, y, w, (std::max)(1.f, D(3.f))}, Faded(kAccent, fade));
  p.Text(x + (w - titleW) * 0.5f, y + D(12.f), g_title, Role::Title, Faded(kPrimary, fade));
  p.Text(x + (w - subW) * 0.5f, y + D(12.f) + lt + D(2.f), g_subtitle, Role::Subtitle, Faded(kSecondary, fade));
}

}  // namespace popup
