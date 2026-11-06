#pragma once
#include "ShellModel.h"
#include <Adafruit_GFX.h>
namespace shell {
struct Status { bool connected=false, speaking=false, clockValid=false; char clock[6]="--:--",date[32]="Waiting for time"; };
// Rendering is allocation-free and shared by device and host previews.
size_t render(Adafruit_GFX &display,const Model &model,const Navigation &nav,const Status &status);
enum class Action { None, Redraw, Face, Settings };
void renderRecent(Adafruit_GFX &,const Model *);
Action tapRecent(const Model *,Navigation &,int x,int y);
Action tap(Model &model,Navigation &nav,int x,int y,size_t nextTextPage);
}
