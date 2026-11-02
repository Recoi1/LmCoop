#pragma once
// The LmCoop logo (assets/lm_coop_logo.png, embedded): decoded once into an ImGui texture.
#include <imgui.h>

namespace setup {

// Null when the image could not be decoded (the screens then show the name as text).
ImTextureID logoTexture(int* width, int* height);

// The logo centered at the top of the current window, as wide as 55% of it and at most 40% of its
// height tall; then the cursor below it. Falls back to a centered "LmCoop" text line.
void drawLogo();

// One centered line of text in the given color.
void centeredText(const char* text, ImVec4 color = ImVec4(1, 1, 1, 1));

}  // namespace setup
