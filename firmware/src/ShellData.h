#pragma once
#include "ShellModel.h"
extern "C" {
#include "cJSON.h"
}
namespace shell {
// Caller supplies a staging model. Commit only when this returns nullptr.
const char *update(Model &staging,View view,const cJSON *params);
const char *context(Model &staging,const cJSON *params);
}
