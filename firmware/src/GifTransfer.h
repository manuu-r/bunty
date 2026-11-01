#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace muse {
// Allocation-free protocol core. Caller owns the PSRAM buffer and serializes access.
class GifTransfer {
 public:
  static constexpr size_t MaxBytes=1048576, ChunkBytes=6144;
  static constexpr uint32_t IdleMs=30000, TotalMs=300000;
  static bool validId(const char *id) {
    if(!id)return false;
    const size_t n=strnlen(id,33);if(!n||n>32)return false;
    for(size_t i=0;i<n;++i)if(!((id[i]>='a'&&id[i]<='z')||(id[i]>='A'&&id[i]<='Z')||
      (id[i]>='0'&&id[i]<='9')||id[i]=='-'||id[i]=='_'))return false;
    return true;
  }
  static bool parseCrc(const char *text,uint32_t &out) {
    if(!text||strnlen(text,9)!=8)return false;
    out=0;for(int i=0;i<8;++i) {
      const char c=text[i];const int v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
      if(v<0)return false;
      out=(out<<4)|unsigned(v);
    }return true;
  }
  // Fast one-shot path: the transport already carries the complete file.
  // No transfer session or CRC is needed; invalid candidates are discarded.
  static size_t inlineCapacity(const char *encoded) {
    if(!encoded)return 0;
    const size_t n=strnlen(encoded,ChunkBytes/3*4+1);
    return n&&n<=ChunkBytes/3*4&&n%4==0?n/4*3:0;
  }
  static const char *decodeInline(const char *encoded,uint8_t *out,size_t capacity,size_t &bytes) {
    bytes=0;const size_t allocation=inlineCapacity(encoded);
    if(!allocation||!out||allocation>capacity)return "Invalid base64 length";
    const size_t n=allocation/3*4;
    for(size_t i=0;i<n;i+=4) {
      uint8_t decoded[3];const int count=decode(encoded+i,i+4==n,decoded);
      if(count<0)return "Invalid base64 data";
      for(int j=0;j<count;++j)out[bytes++]=decoded[j];
    }
    return nullptr;
  }
  bool active() const {return data_!=nullptr;}
  bool matches(const char *id) const {return active()&&id&&!strcmp(id,id_);}
  bool expired(uint32_t now) const {return active()&&(uint32_t(now-last_)>=IdleMs||uint32_t(now-start_)>=TotalMs);}
  bool same(const char *id,size_t size,uint32_t crc,int row) const {
    return matches(id)&&size==size_&&crc==expected_&&row==row_;
  }
  void begin(uint8_t *buffer,const char *id,size_t size,uint32_t crc,int row,uint32_t now) {
    data_=buffer;strcpy(id_,id);size_=size;expected_=crc;row_=row;received_=0;crc_=0xffffffff;
    start_=last_=now;
  }
  const char *append(const char *id,size_t offset,const char *encoded,uint32_t now) {
    if(!matches(id))return "Unknown transfer";
    if(expired(now))return "Transfer expired";
    if(!encoded)return "Missing base64 data";
    const size_t n=strnlen(encoded,ChunkBytes/3*4+1);
    if(!n||n>ChunkBytes/3*4||n%4)return "Invalid base64 length";
    size_t decoded=0;
    // Validate before mutating the buffer; accept only canonical padded base64.
    for(size_t i=0;i<n;i+=4) {
      uint8_t bytes[3];int count=decode(encoded+i,i+4==n,bytes);
      if(count<0)return "Invalid base64 data";
      decoded+=size_t(count);
    }
    if(offset>received_||decoded>size_-offset)return "Chunk offset or size out of bounds";
    const bool duplicate=offset<received_;
    if(duplicate&&decoded>received_-offset)return "Chunk overlaps received boundary";
    size_t at=offset;
    if(duplicate) {
      for(size_t i=0;i<n;i+=4) {uint8_t bytes[3];int count=decode(encoded+i,i+4==n,bytes);
        for(int j=0;j<count;++j)if(data_[at++]!=bytes[j])return "Conflicting duplicate chunk";
      }
      // Duplicate retries do not extend the timeout indefinitely.
      return nullptr;
    }
    for(size_t i=0;i<n;i+=4) {uint8_t bytes[3];int count=decode(encoded+i,i+4==n,bytes);
      for(int j=0;j<count;++j) {data_[at++]=bytes[j];crc_^=bytes[j];
        for(int k=0;k<8;++k)crc_=(crc_>>1)^(0xedb88320u&uint32_t(-int32_t(crc_&1)));
      }
    }
    received_=at;last_=now;return nullptr;
  }
  const char *ready(const char *id,uint32_t now) const {
    if(!matches(id))return "Unknown transfer";
    if(expired(now))return "Transfer expired";
    if(received_!=size_)return "Transfer incomplete";
    if((crc_^0xffffffff)!=expected_)return "GIF checksum mismatch";
    return nullptr;
  }
  uint8_t *release() {uint8_t *value=data_;data_=nullptr;id_[0]=0;size_=received_=0;return value;}
  size_t received() const {return received_;}
  size_t size() const {return size_;}
  uint32_t checksum() const {return expected_;}
  int row() const {return row_;}
 private:
  static int value(char c) {
    return c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='+'?62:c=='/'?63:-1;
  }
  static int decode(const char *p,bool last,uint8_t *out) {
    int a=value(p[0]),b=value(p[1]),c=value(p[2]),d=value(p[3]);
    if(a<0||b<0)return -1;
    out[0]=uint8_t((a<<2)|(b>>4));
    if(p[2]=='=')return last&&p[3]=='='&&!(b&15)?1:-1;
    if(c<0)return -1;
    out[1]=uint8_t((b<<4)|(c>>2));
    if(p[3]=='=')return last&&!(c&3)?2:-1;
    if(d<0)return -1;
    out[2]=uint8_t((c<<6)|d);return 3;
  }
  uint8_t *data_=nullptr;
  char id_[33]={};
  size_t size_=0,received_=0;
  uint32_t crc_=0xffffffff,expected_=0,start_=0,last_=0;
  int row_=-1;
};
} // namespace muse
