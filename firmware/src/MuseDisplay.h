#pragma once
#include "Runtime.h"
#include <cstddef>
namespace muse {
bool beginDisplay();
void serviceDisplay();
const char *showGifBase64(const char *data,int row=-1);
struct GifTransferReply {const char *error;size_t received,total;bool active,displayed;};
GifTransferReply transferGif(const char *op,const char *id,size_t bytes,uint32_t crc,int row,size_t offset,const char *data);
bool displayActive();
bool imageFetching();
bool claimTtsNetwork();
void releaseTtsNetwork();
void dismissDisplay();
// A tap pages long text; on the final page or an image, it restores the face.
void tapDisplay();
bool showText(const char *text,const char *title,uint16_t foreground,uint16_t background);
}
