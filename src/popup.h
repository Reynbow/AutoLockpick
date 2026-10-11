#pragma once

// A short popup at the top of the screen (the mod switched on or off), drawn with the engine's own
// canvas in the same style as TrueFOV's.

namespace popup {

void Show(const wchar_t* title, const wchar_t* subtitle);
// Called once per frame on the game thread with the viewport's canvas.
void Draw(void* canvas, double now);

}  // namespace popup
