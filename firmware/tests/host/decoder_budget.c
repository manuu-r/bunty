// Compile with the ESP32-S3 toolchain and inspect this symbol's size to audit
// the exact decoder allocation requested by the pinned Helix version.
#include "coder.h"
unsigned char bunty_decoder_workspace_budget[
    sizeof(MP3DecInfo) + sizeof(FrameHeader) + sizeof(SideInfo) +
    sizeof(ScaleFactorInfo) + sizeof(HuffmanInfo) + sizeof(DequantInfo) +
    sizeof(IMDCTInfo) + sizeof(SubbandInfo)];
