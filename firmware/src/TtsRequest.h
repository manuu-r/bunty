#pragma once
#include <cstddef>
#include <cstring>
namespace audio {
// Worst case: every input byte is a JSON control escape (six output bytes).
constexpr size_t kTtsBodyCapacity=1023*6+12;
inline size_t ttsRequest(const char *text,char *out,size_t capacity) {
  if(!text || !out || !capacity) return 0;
  size_t used=0;
  const auto append=[&](const char *data,size_t length) {
    if(length>=capacity-used) return false;
    memcpy(out+used,data,length); used+=length; out[used]=0; return true;
  };
  out[0]=0;
  if(!append("{\"text\":\"",9)) return 0;
  for(const auto *p=reinterpret_cast<const unsigned char *>(text);*p;++p) {
    if(*p<32) {
      constexpr char hex[]="0123456789abcdef";
      const char escaped[]={'\\','u','0','0',hex[*p>>4],hex[*p&15]};
      if(!append(escaped,sizeof(escaped))) return 0;
    } else if(*p=='"' || *p=='\\') {
      const char escaped[]={'\\',char(*p)};
      if(!append(escaped,sizeof(escaped))) return 0;
    } else if(!append(reinterpret_cast<const char *>(p),1)) return 0;
  }
  return append("\"}",2)?used:0;
}
}
