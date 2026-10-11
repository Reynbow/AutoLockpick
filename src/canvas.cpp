#include "canvas.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "game.h"
#include "log.h"

namespace canvas {
namespace {

using game::F;
using game::O;
using ue::At;
using ue::FColor;
using ue::FLinearColor;
using ue::FString;
using ue::FVector2D;
using ue::Ptr;

constexpr uint8_t kBlendTranslucent = 2;  // EBlendMode::BLEND_Translucent
constexpr uint8_t kRuntimeFontCache = 1;
constexpr float kRoleLineDp[] = {40.f, 20.f};
static_assert(sizeof(kRoleLineDp) / sizeof(kRoleLineDp[0]) == static_cast<int>(Role::Count), "one size per role");
constexpr float kMinLinePx = 14.f;
constexpr int32_t kMaxParams = 256;

// Where each argument goes in the parameter blocks of the canvas functions.
struct Params {
  bool ready = false;
  int32_t texTexture = -1, texPos = -1, texSize = -1, texCoordPos = -1, texCoordSize = -1, texColor = -1,
          texBlend = -1, texRotation = -1, texPivot = -1;
  int32_t txtFont = -1, txtText = -1, txtPos = -1, txtScale = -1, txtColor = -1, txtShadowOffset = -1;
  int32_t sizeFont = -1, sizeText = -1, sizeScale = -1;
};
Params g_p;
bool g_paramsChecked = false;

bool InitParams() {
  if (g_paramsChecked) return g_p.ready;
  g_paramsChecked = true;
  const ue::Function& t = F.K2DrawTexture;
  g_p.texTexture = t.Param("RenderTexture");
  g_p.texPos = t.Param("ScreenPosition");
  g_p.texSize = t.Param("ScreenSize");
  g_p.texCoordPos = t.Param("CoordinatePosition");
  g_p.texCoordSize = t.Param("CoordinateSize");
  g_p.texColor = t.Param("RenderColor");
  g_p.texBlend = t.Param("BlendMode");
  g_p.texRotation = t.Param("Rotation");
  g_p.texPivot = t.Param("PivotPoint");
  const ue::Function& d = F.K2DrawText;
  g_p.txtFont = d.Param("RenderFont");
  g_p.txtText = d.Param("RenderText");
  g_p.txtPos = d.Param("ScreenPosition");
  g_p.txtScale = d.Param("Scale");
  g_p.txtColor = d.Param("RenderColor");
  g_p.txtShadowOffset = d.Param("ShadowOffset");
  const ue::Function& s = F.K2TextSize;
  g_p.sizeFont = s.Param("RenderFont");
  g_p.sizeText = s.Param("RenderText");
  g_p.sizeScale = s.Param("Scale");
  g_p.ready = g_p.texPos >= 0 && g_p.texSize >= 0 && g_p.texCoordPos >= 0 && g_p.texCoordSize >= 0 &&
              g_p.texColor >= 0 && g_p.txtFont >= 0 && g_p.txtText >= 0 && g_p.txtPos >= 0 && g_p.txtScale >= 0 &&
              g_p.txtColor >= 0 && g_p.sizeFont >= 0 && g_p.sizeText >= 0 && g_p.sizeScale >= 0 && s.Return() >= 0 &&
              t.ParamsSize() <= kMaxParams && d.ParamsSize() <= kMaxParams && s.ParamsSize() <= kMaxParams;
  if (!g_p.ready) LOG("Canvas functions have unexpected parameters: the popup can't be drawn");
  return g_p.ready;
}

FString View(const wchar_t* s) {
  const auto n = static_cast<int32_t>(wcslen(s)) + 1;
  return {s, n, n};
}

// Text colours are given in sRGB like the rest of the palette; the canvas takes linear colour.
FLinearColor Linear(FColor c) {
  static float table[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; ++i) {
      const float v = i / 255.f;
      table[i] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    }
    init = true;
  }
  return {table[c.R], table[c.G], table[c.B], c.A / 255.f};
}

// The pending LegacyFontSize write, so EmergencyRestore can undo it after a fault mid-draw.
int32_t* g_sizeSlot = nullptr;
int32_t g_sizeOld = 0;

class SizeScope {
 public:
  SizeScope(void* font, int32_t size) {
    if (!font || size <= 0 || O.Font_LegacyFontSize < 0) return;
    slot_ = &At<int32_t>(font, O.Font_LegacyFontSize);
    old_ = *slot_;
    *slot_ = size;
    g_sizeSlot = slot_;
    g_sizeOld = old_;
  }
  ~SizeScope() {
    if (!slot_) return;
    *slot_ = old_;
    g_sizeSlot = nullptr;
  }
  SizeScope(const SizeScope&) = delete;
  SizeScope& operator=(const SizeScope&) = delete;

 private:
  int32_t* slot_ = nullptr;
  int32_t old_ = 0;
};

FVector2D Measure(void* canvas, void* font, const wchar_t* s) {
  alignas(16) uint8_t p[kMaxParams] = {};
  At<void*>(p, g_p.sizeFont) = font;
  At<FString>(p, g_p.sizeText) = View(s);
  At<FVector2D>(p, g_p.sizeScale) = {1.0, 1.0};
  F.K2TextSize.Call(canvas, p);
  return At<FVector2D>(p, F.K2TextSize.Return());
}

// Line height ("Ag" measures the font's full line height) at |size| points; 0 keeps the font's own size.
float MeasureLine(void* canvas, void* font, int32_t size) {
  SizeScope scope(font, size);
  return static_cast<float>(Measure(canvas, font, L"Ag").Y);
}

// Prefers an engine font that is rasterized at runtime (resizable); otherwise the first one found.
void* PickFont(void* engine, bool& runtime, const char*& slotName) {
  const struct {
    int32_t offset;
    const char* name;
  } slots[] = {{O.Eng_MediumFont, "MediumFont"},
               {O.Eng_LargeFont, "LargeFont"},
               {O.Eng_SubtitleFont, "SubtitleFont"},
               {O.Eng_SmallFont, "SmallFont"}};
  void* fallback = nullptr;
  const char* fallbackName = "";
  const bool canResize = O.Font_CacheType >= 0 && O.Font_LegacyFontSize >= 0;
  for (const auto& slot : slots) {
    if (slot.offset < 0) continue;
    void* font = Ptr(engine, slot.offset);
    if (!font) continue;
    if (!fallback) {
      fallback = font;
      fallbackName = slot.name;
    }
    if (canResize && At<uint8_t>(font, O.Font_CacheType) == kRuntimeFontCache) {
      runtime = true;
      slotName = slot.name;
      return font;
    }
  }
  runtime = false;
  slotName = fallbackName;
  return fallback;
}

// FCanvas::GetDPIScale(): the Windows display scaling the debug canvas works in (1.5 at 150%).
float ReadDpiScale(void* canvas) {
  __try {
    void* fc = Ptr(canvas, O.Canvas_Canvas);
    if (!fc) return 1.f;
    const bool stereo = At<uint8_t>(fc, O.FCanvas_Stereo) != 0;
    const float d = At<float>(fc, O.FCanvas_DPIScale);
    if (!stereo && d >= 0.5f && d <= 8.f) return d;  // false for NaN too
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return 1.f;
}

struct Calibration {
  void* font = nullptr;
  float nativeLine = 0.f;  // line height at the font's own size
  float perPoint = 0.f;    // line height per point, runtime fonts only
};
Calibration g_calibration;

struct SizeCache {
  void* font = nullptr;
  float unit = 0.f;
  float dpi = 0.f;
  int32_t size[static_cast<int>(Role::Count)] = {};
  float line[static_cast<int>(Role::Count)] = {};
};
SizeCache g_sizes;

}  // namespace

bool Painter::Begin(void* canvas, float unit) {
  End();
  void* engine = game::Engine();
  if (!canvas || !engine || !InitParams()) return false;
  width_ = static_cast<float>(At<int32_t>(canvas, O.Canvas_SizeX));
  height_ = static_cast<float>(At<int32_t>(canvas, O.Canvas_SizeY));
  if (width_ < 64.f || height_ < 64.f) return false;
  const char* slot = "";
  font_ = PickFont(engine, runtimeFont_, slot);
  if (!font_) return false;
  canvas_ = canvas;
  white_ = O.Canvas_DefaultTexture >= 0 ? Ptr(canvas, O.Canvas_DefaultTexture) : nullptr;

  // The debug canvas works in DPI-scaled units: the DPI scale is baked into its base transform, so
  // every position and size is multiplied by it, while UCanvas::SizeX/SizeY stay in physical
  // pixels. The popup lays out in physical pixels and divides by the scale when drawing.
  // Runtime-font glyphs are rasterized at size * DPI scale.
  dpi_ = ReadDpiScale(canvas);
  const bool dpiGlyphs = O.Font_CacheType < 0 || At<uint8_t>(font_, O.Font_CacheType) == kRuntimeFontCache;
  sizeDpi_ = dpiGlyphs ? dpi_ : 1.f;

  if (g_calibration.font != font_) {
    g_calibration = {};
    g_calibration.font = font_;
    g_calibration.nativeLine = MeasureLine(canvas, font_, 0);
    if (runtimeFont_) g_calibration.perPoint = MeasureLine(canvas, font_, 32) / 32.f;
    LOG("Popup font: GEngine->%s, %s, native size %d (line %.1f px), %.3f px per point, DPI scale %.2f, canvas %.0fx%.0f",
        slot, runtimeFont_ ? "runtime (resizable)" : "bitmap (fixed size)",
        O.Font_LegacyFontSize >= 0 ? At<int32_t>(font_, O.Font_LegacyFontSize) : -1, g_calibration.nativeLine,
        g_calibration.perPoint, dpi_, width_, height_);
    g_sizes.font = nullptr;
  }
  if (g_sizes.font != font_ || g_sizes.unit != unit || g_sizes.dpi != dpi_) {
    g_sizes.font = font_;
    g_sizes.unit = unit;
    g_sizes.dpi = dpi_;
    for (int r = 0; r < static_cast<int>(Role::Count); ++r) {
      const float target = (std::max)(kMinLinePx, kRoleLineDp[r] * unit);
      if (runtimeFont_ && g_calibration.perPoint > 0.1f) {
        const int32_t size = std::clamp(static_cast<int32_t>(std::lround(target / (g_calibration.perPoint * dpi_))), 6, 200);
        g_sizes.size[r] = size;
        g_sizes.line[r] = MeasureLine(canvas, font_, size) * dpi_;
      } else {
        g_sizes.size[r] = 0;
        g_sizes.line[r] = g_calibration.nativeLine * sizeDpi_;
      }
    }
  }
  std::copy(std::begin(g_sizes.size), std::end(g_sizes.size), size_);
  std::copy(std::begin(g_sizes.line), std::end(g_sizes.line), lineH_);
  begun_ = true;
  return true;
}

void Painter::End() { begun_ = false; }

void Painter::Fill(const Rect& r, FLinearColor color) const {
  if (!begun_ || r.w <= 0.f || r.h <= 0.f) return;
  // Edges on whole physical pixels, then into canvas units.
  const float x0 = std::round(r.x), y0 = std::round(r.y);
  const float x1 = (std::max)(x0 + 1.f, std::round(r.x + r.w)), y1 = (std::max)(y0 + 1.f, std::round(r.y + r.h));
  const double s = 1.0 / dpi_;
  alignas(16) uint8_t p[kMaxParams] = {};
  if (g_p.texTexture >= 0) At<void*>(p, g_p.texTexture) = white_;
  At<FVector2D>(p, g_p.texPos) = {x0 * s, y0 * s};
  At<FVector2D>(p, g_p.texSize) = {(x1 - x0) * s, (y1 - y0) * s};
  At<FVector2D>(p, g_p.texCoordPos) = {0.0, 0.0};
  At<FVector2D>(p, g_p.texCoordSize) = {1.0, 1.0};
  At<FLinearColor>(p, g_p.texColor) = color;
  if (g_p.texBlend >= 0) At<uint8_t>(p, g_p.texBlend) = kBlendTranslucent;
  if (g_p.texPivot >= 0) At<FVector2D>(p, g_p.texPivot) = {0.5, 0.5};
  F.K2DrawTexture.Call(canvas_, p);
}

void Painter::Outline(const Rect& r, FLinearColor color) const {
  const float x0 = std::round(r.x), y0 = std::round(r.y);
  const float w = std::round(r.x + r.w) - x0, h = std::round(r.y + r.h) - y0;
  if (w < 2.f || h < 2.f) return;
  Fill({x0, y0, w, 1.f}, color);
  Fill({x0, y0 + h - 1.f, w, 1.f}, color);
  Fill({x0, y0 + 1.f, 1.f, h - 2.f}, color);
  Fill({x0 + w - 1.f, y0 + 1.f, 1.f, h - 2.f}, color);
}

void Painter::Text(float x, float y, const wchar_t* s, Role role, FColor color) const {
  if (!begun_ || !s || !*s) return;
  SizeScope scope(runtimeFont_ ? font_ : nullptr, size_[static_cast<int>(role)]);
  alignas(16) uint8_t p[kMaxParams] = {};
  At<void*>(p, g_p.txtFont) = font_;
  At<FString>(p, g_p.txtText) = View(s);
  At<FVector2D>(p, g_p.txtPos) = {TextCoord(x), TextCoord(y)};
  At<FVector2D>(p, g_p.txtScale) = {1.0, 1.0};
  At<FLinearColor>(p, g_p.txtColor) = Linear(color);
  // ShadowColor, ShadowOffset, bOutlined...: zero (no shadow, no outline).
  F.K2DrawText.Call(canvas_, p);
}

// Text starts on a whole physical pixel: the glyph atlas is sampled bilinearly, so a glyph half a
// pixel off looks blurry.
float Painter::TextCoord(float physical) const { return std::round(physical) / dpi_; }

void Painter::TextCentered(const Rect& r, const wchar_t* s, Role role, FColor color) const {
  Text(r.x + (r.w - TextWidth(s, role)) * 0.5f, r.y + (r.h - LineHeight(role)) * 0.5f, s, role, color);
}

float Painter::TextWidth(const wchar_t* s, Role role) const {
  if (!begun_ || !s || !*s) return 0.f;
  SizeScope scope(runtimeFont_ ? font_ : nullptr, size_[static_cast<int>(role)]);
  return static_cast<float>(Measure(canvas_, font_, s).X) * sizeDpi_;
}

void EmergencyRestore() {
  if (g_sizeSlot) {
    *g_sizeSlot = g_sizeOld;
    g_sizeSlot = nullptr;
  }
}

}  // namespace canvas
