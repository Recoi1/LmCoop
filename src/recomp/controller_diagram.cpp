#include "recomp/controller_diagram.h"

#include <dolphin/pad.h>
#include <tuple>

namespace recomp::controls {
void controllerDiagram(ImVec2 size, const PADStatus* state) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* d = ImGui::GetWindowDrawList();
    auto p = [&](float x, float y) { return ImVec2(origin.x + x * size.x, origin.y + y * size.y); };
    auto pressed = [&](unsigned bit) { return state && (state->button & bit); };
    const ImU32 shell = IM_COL32(216, 220, 230, 255), edge = IM_COL32(108, 117, 139, 255);
    const float unit = size.x;
    // A curved GameCube shell, including the space between the two handles.
    auto outline = [&] {
        d->PathLineTo(p(.27f, .22f));
        d->PathBezierCubicCurveTo(p(.18f, .18f), p(.10f, .29f), p(.08f, .43f));
        d->PathBezierCubicCurveTo(p(.04f, .62f), p(.01f, .87f), p(.12f, .90f));
        d->PathBezierCubicCurveTo(p(.20f, .94f), p(.25f, .70f), p(.34f, .65f));
        d->PathBezierCubicCurveTo(p(.43f, .60f), p(.57f, .60f), p(.66f, .65f));
        d->PathBezierCubicCurveTo(p(.75f, .70f), p(.80f, .94f), p(.88f, .90f));
        d->PathBezierCubicCurveTo(p(.99f, .87f), p(.96f, .62f), p(.92f, .43f));
        d->PathBezierCubicCurveTo(p(.90f, .29f), p(.82f, .18f), p(.73f, .22f));
        d->PathBezierCubicCurveTo(p(.57f, .17f), p(.43f, .17f), p(.27f, .22f));
    };
    outline(); d->PathFillConcave(shell);
    outline(); d->PathStroke(edge, ImDrawFlags_Closed, 1.5f);
    d->AddRectFilled(p(.20f, .17f), p(.35f, .24f), pressed(PAD_TRIGGER_L) ? IM_COL32(90, 194, 246, 255) : edge, unit * .025f);
    d->AddRectFilled(p(.65f, .17f), p(.80f, .24f), pressed(PAD_TRIGGER_R) ? IM_COL32(90, 194, 246, 255) : edge, unit * .025f);
    auto button = [&](float x, float y, float r, ImU32 color, const char* label, unsigned bit) {
        const auto center = p(x, y);
        d->AddCircleFilled(center, unit * r + 2, pressed(bit) ? IM_COL32(117, 218, 255, 255) : edge);
        d->AddCircleFilled(center, unit * r, color);
        const ImVec2 text = ImGui::CalcTextSize(label);
        d->AddText(ImVec2(center.x - text.x * .5f, center.y - text.y * .5f), IM_COL32(30, 35, 47, 255), label);
    };
    button(.76f, .43f, .066f, IM_COL32(71, 196, 143, 255), "A", PAD_BUTTON_A);
    button(.66f, .54f, .038f, IM_COL32(230, 91, 100, 255), "B", PAD_BUTTON_B);
    button(.88f, .38f, .036f, IM_COL32(160, 168, 185, 255), "X", PAD_BUTTON_X);
    button(.72f, .29f, .036f, IM_COL32(160, 168, 185, 255), "Y", PAD_BUTTON_Y);
    button(.85f, .22f, .032f, IM_COL32(138, 118, 211, 255), "Z", PAD_TRIGGER_Z);
    button(.50f, .45f, .023f, IM_COL32(154, 163, 182, 255), "", PAD_BUTTON_START);
    auto stick = [&](float x, float y, float r, float sx, float sy, ImU32 color) {
        d->AddCircleFilled(p(x, y), unit * r + 4, edge, 8);
        d->AddCircleFilled(p(x, y), unit * r, IM_COL32(45, 52, 69, 255), 32);
        const ImVec2 center = p(x + sx * r * .6f, y - sy * r * .6f);
        d->AddCircleFilled(center, unit * r * .67f, color, 24);
        d->AddCircle(center, unit * r * .44f, IM_COL32(255, 255, 255, 45), 24, 1.0f);
    };
    stick(.27f, .39f, .078f, state ? state->stickX / 127.f : 0, state ? state->stickY / 127.f : 0,
          IM_COL32(133, 142, 159, 255));
    stick(.63f, .68f, .056f, state ? state->substickX / 127.f : 0, state ? state->substickY / 127.f : 0,
          IM_COL32(236, 194, 63, 255));
    const ImU32 cross = IM_COL32(76, 85, 105, 255);
    d->AddRectFilled(p(.31f, .60f), p(.36f, .78f), cross, unit * .006f);
    d->AddRectFilled(p(.26f, .66f), p(.41f, .72f), cross, unit * .006f);
    for (auto [x, y, bit] : {std::tuple{.335f, .625f, PAD_BUTTON_UP}, {.335f, .755f, PAD_BUTTON_DOWN},
                           {.28f, .69f, PAD_BUTTON_LEFT}, {.39f, .69f, PAD_BUTTON_RIGHT}})
        if (pressed(bit)) d->AddCircleFilled(p(x, y), unit * .012f, IM_COL32(117, 218, 255, 255));
    ImGui::Dummy(size);
}
}
