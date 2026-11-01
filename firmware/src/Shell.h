#pragma once
#include "ShellView.h"
#include "Touch.h"
extern "C" {
#include "cJSON.h"
}
namespace shell {
bool begin();
bool visible();
bool resume();
void hide();
void showHome();
void showFace();
bool takeFaceRequest();
void invalidate();
uint32_t activityAt();
void wake(uint32_t now);
Action service(const touch::Event &event,bool connected,bool speaking,uint32_t now);
void registerCommands(cJSON *commands);
cJSON *command(const char *name,const cJSON *params);
}
