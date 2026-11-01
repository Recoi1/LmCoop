#pragma once
#include "recomp/ctx.h"
struct PADStatus;
namespace recomp::campaign {
bool hold();
void pad(PADStatus* pads);
void snapshot();
}
