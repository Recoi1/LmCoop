#pragma once
#include <imgui.h>
struct PADStatus;

namespace recomp::controls {
void controllerDiagram(ImVec2 size, const PADStatus* state);
}
