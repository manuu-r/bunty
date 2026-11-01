#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace muse {
inline bool screenColor(const char *value,uint16_t &color) {
  if(!value || std::strlen(value)!=7 || value[0]!='#') return false;
  unsigned rgb=0;
  for(size_t i=1;i<7;++i) {
    const char ch=value[i];
    const int digit=ch>='0' && ch<='9'?ch-'0':ch>='a' && ch<='f'?ch-'a'+10:ch>='A' && ch<='F'?ch-'A'+10:-1;
    if(digit<0) return false;
    rgb=(rgb<<4)|unsigned(digit);
  }
  color=uint16_t(((rgb>>19)&31)<<11 | ((rgb>>10)&63)<<5 | ((rgb>>3)&31));
  return true;
}
// The bundled bitmap font is ASCII. Consume complete UTF-8 sequences and use
// one '?' per unsupported code point, rather than leaking bytes into glyphs.
inline size_t screenAscii(const char *input,char *output,size_t capacity) {
  if(!capacity) return 0;
  size_t used=0;
  while(*input && used+1<capacity) {
    const unsigned char ch=static_cast<unsigned char>(*input++);
    if(ch=='\r') continue;
    if(ch=='\t') { output[used++]=' '; continue; }
    if(ch>=32 && ch<127) { output[used++]=char(ch); continue; }
    if(ch=='\n') { output[used++]='\n'; continue; }
    if(ch<128) continue;
    output[used++]='?';
    while((static_cast<unsigned char>(*input)&0xc0)==0x80) ++input;
  }
  output[used]=0; return used;
}
// Wrap by actual font width, preserving explicit newlines and progress on long
// words. Returns the next page's byte offset, including all skipped whitespace.
// Line receives a bounded, null-terminated scratch buffer only during the call.
template<class Width,class Line>
size_t textPage(const char *text,size_t offset,int maxWidth,size_t maxLines,
                Width width,Line line) {
  const size_t length=std::strlen(text);
  if(offset>length) offset=length;
  for(size_t row=0;row<maxLines && offset<length;++row) {
    if(text[offset]=='\n') { line(row,""); ++offset; continue; }
    while(offset<length && text[offset]==' ') ++offset;
    if(offset==length) break;
    if(text[offset]=='\n') { line(row,""); ++offset; continue; }
    char buffer[128]={}; size_t count=0,lastSpace=0;
    while(offset+count<length && text[offset+count]!='\n' && count+1<sizeof(buffer)) {
      buffer[count]=text[offset+count]; buffer[count+1]=0;
      if(width(buffer)>maxWidth) { buffer[count]=0; break; }
      if(buffer[count]==' ') lastSpace=count;
      ++count;
    }
    if(count==0) { buffer[0]=text[offset]; buffer[1]=0; count=1; }
    const bool wrap=offset+count<length && text[offset+count]!='\n';
    if(wrap && lastSpace) { count=lastSpace; buffer[count]=0; }
    line(row,buffer); offset+=count;
    while(offset<length && text[offset]==' ') ++offset;
    if(offset<length && text[offset]=='\n') ++offset;
  }
  return offset;
}
}  // namespace muse
