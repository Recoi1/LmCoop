// LmCoop logo and the centered layout helpers of the setup and start screens.
#include "setup/logo.h"

#include "setup/embedded.h"

#include <aurora/imgui.h>
#include <png.h>

#include <algorithm>
#include <vector>

namespace setup {

ImTextureID logoTexture(int* width, int* height) {
    static ImTextureID sTex{};
    static int sW = 0, sH = 0;
    static bool sTried = false;
    if (!sTried) {
        sTried = true;
        const auto file = embedded::logo();
        png_image img{};
        img.version = PNG_IMAGE_VERSION;
        if (png_image_begin_read_from_memory(&img, file.data(), file.size())) {
            img.format = PNG_FORMAT_RGBA;
            std::vector<unsigned char> rgba(PNG_IMAGE_SIZE(img));
            if (png_image_finish_read(&img, nullptr, rgba.data(), 0, nullptr)) {
                sTex = aurora_imgui_add_texture(img.width, img.height, rgba.data());
                sW = int(img.width), sH = int(img.height);
            } else {
                png_image_free(&img);
            }
        }
    }
    if (width) *width = sW;
    if (height) *height = sH;
    return sTex;
}

void drawLogo() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float winH = ImGui::GetWindowHeight();
    int w = 0, h = 0;
    if (const ImTextureID tex = logoTexture(&w, &h); tex && w > 0 && h > 0) {
        float dw = avail.x * 0.55f, dh = dw * float(h) / float(w);
        if (dh > winH * 0.40f) dh = winH * 0.40f, dw = dh * float(w) / float(h);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - dw) * 0.5f);
        ImGui::Image(tex, ImVec2(dw, dh));
    } else {
        ImGui::SetWindowFontScale(3.0f);
        centeredText("LmCoop");
        ImGui::SetWindowFontScale(1.0f);
    }
}

void centeredText(const char* text, ImVec4 color) {
    const float wrap = ImGui::GetWindowWidth() * 0.9f;
    const ImVec2 size = ImGui::CalcTextSize(text, nullptr, false, wrap);
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - size.x) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

}  // namespace setup
