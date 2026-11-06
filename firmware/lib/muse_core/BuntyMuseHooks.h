#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif
void bunty_muse_register(cJSON *commands);
cJSON *bunty_muse_command(const char *name, cJSON *params);
void bunty_muse_reply(const char *text);
void bunty_muse_agent_busy(bool busy);
void bunty_muse_agent_activity(const char *activity);
bool bunty_muse_parse_image_gesture(cJSON *params,int *kind,int *speed);
void bunty_muse_image_gesture(int kind,int speed);
bool bunty_muse_chat_init(void);
bool bunty_muse_image_begin(void);
bool bunty_muse_image_current(void);
bool bunty_muse_image_end(bool success);
typedef int (*bunty_gif_read)(void *,uint8_t *,size_t);
const char *bunty_muse_gif_load(bunty_gif_read read,void *context,int row,int *width,int *height);
#ifdef __cplusplus
}
#endif
