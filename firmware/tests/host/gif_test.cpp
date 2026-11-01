#include "GifCanvas.h"
#include "GifDownload.h"
#include "GifTransfer.h"
#include <cstdlib>
#include "MuseTurnError.h"
#include <cassert>
#include <vector>
#include <iostream>
#include <fstream>
#include <iterator>
using Bytes=std::vector<uint8_t>;
void word(Bytes &b,unsigned n){b.push_back(n&255);b.push_back(n>>8);}
Bytes header(bool loop=true) {
  Bytes b={'G','I','F','8','9','a',3,0,2,0,0x81,0,0,
           0,0,0,255,0,0,0,255,0,0,0,255};
  if(loop){const uint8_t ext[]={0x21,0xff,11,'N','E','T','S','C','A','P','E','2','.','0',3,1,0,0,0};b.insert(b.end(),ext,ext+sizeof(ext));}
  return b;
}
void frame(Bytes &b,int x,int y,int w,int h,const Bytes &pixels,int disposal=1,bool transparent=false,bool local=false,bool interlaced=false) {
  const uint8_t gce[]={0x21,0xf9,4,uint8_t(disposal*4+(transparent?1:0)),1,0,0,0};b.insert(b.end(),gce,gce+8);
  b.push_back(0x2c);word(b,x);word(b,y);word(b,w);word(b,h);b.push_back((local?0x81:0)|(interlaced?0x40:0));
  if(local){const uint8_t colors[]={0,0,0,255,255,255,0,255,0,0,0,255};b.insert(b.end(),colors,colors+12);}
  b.push_back(2);Bytes packed;unsigned bits=0,value=0;
  auto code=[&](unsigned c){value|=c<<bits;bits+=3;while(bits>=8){packed.push_back(value&255);value>>=8;bits-=8;}};
  for(auto pixel:pixels){code(4);code(pixel);}code(5);if(bits)packed.push_back(value&255);
  b.push_back(uint8_t(packed.size()));b.insert(b.end(),packed.begin(),packed.end());b.push_back(0);
}
struct Download {
  Bytes input;size_t at=0,fragment=4096,allocated=0;bool fail=false,readFail=false;
  int read(uint8_t *out,size_t n) {
    if(readFail)return -1;
    n=std::min(n,std::min(fragment,input.size()-at));
    if(n)memcpy(out,input.data()+at,n);
    at+=n;return int(n);
  }
  uint8_t *resize(uint8_t *data,size_t n) {
    if(fail)return nullptr;
    allocated=n;return static_cast<uint8_t *>(realloc(data,n));
  }
};
// Pad with legal comment subblocks, preserving a valid GIF at exact boundaries.
Bytes padded(Bytes b,size_t target) {
  b.pop_back();size_t remaining=target-b.size()-4;
  b.insert(b.end(),{0x21,0xfe});
  while(remaining) {
    size_t count=std::min(size_t(255),remaining-1);
    if(remaining-count-1==1)--count;
    assert(count);b.push_back(uint8_t(count));b.insert(b.end(),count,0);
    remaining-=count+1;
  }
  b.insert(b.end(),{0,0x3b});assert(b.size()==target);return b;
}
void directChecks(const Bytes &valid) {
  const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint32_t crc=0xffffffff;
  for(uint8_t byte:valid) {crc^=byte;for(int k=0;k<8;++k)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
  Bytes received(valid.size());muse::GifTransfer transfer;
  transfer.begin(received.data(),"decoder-check",valid.size(),crc^0xffffffff,-1,0);
  for(size_t offset=0;offset<valid.size();offset+=15) {
    size_t n=std::min(size_t(15),valid.size()-offset);std::string encoded;
    for(size_t i=0;i<n;i+=3) {
      uint32_t v=uint32_t(valid[offset+i])<<16;
      if(i+1<n)v|=uint32_t(valid[offset+i+1])<<8;
      if(i+2<n)v|=valid[offset+i+2];
      encoded+=alphabet[v>>18];encoded+=alphabet[(v>>12)&63];
      encoded+=i+1<n?alphabet[(v>>6)&63]:'=';encoded+=i+2<n?alphabet[v&63]:'=';
    }
    assert(!transfer.append("decoder-check",offset,encoded.c_str(),100));
    assert(!transfer.append("decoder-check",offset,encoded.c_str(),101)); // lost ACK retry
  }
  assert(!transfer.ready("decoder-check",102));assert(received==valid);
  unsigned frames=0;assert(muse::validateGif(received.data(),received.size(),frames));
  std::vector<uint16_t> canvas(240*320),restore(canvas.size());muse::GifCanvas player;
  assert(player.open(transfer.release(),received.size(),canvas.data(),restore.data(),-1,0));
  assert(player.next(0));assert(canvas[159*240+118]==0xf800);
  assert(player.next(100));assert(canvas[159*240+118]==0x07e0);
}
void downloadChecks(const Bytes &valid) {
  for(size_t size:{valid.size(),muse::kGifMaxBytes}) {
    Download ops;ops.input=size==valid.size()?valid:padded(valid,size);ops.fragment=17;
    uint8_t *data=nullptr;size_t bytes=0;
    assert(!muse::downloadGif(ops,data,bytes,muse::kGifMaxBytes));
    assert(bytes==size && ops.allocated==size && !memcmp(data,ops.input.data(),size));
    unsigned count;assert(muse::validateGif(data,bytes,count));free(data);
  }
  Download over;over.input=padded(valid,muse::kGifMaxBytes+1);
  unsigned count;assert(!muse::validateGif(over.input.data(),over.input.size(),count));
  uint8_t *data=nullptr;size_t bytes=0;
  assert(!strcmp(muse::downloadGif(over,data,bytes,muse::kGifMaxBytes),"GIF exceeds 1 MiB"));free(data);
  for(bool readFail:{false,true}) {
    Download ops;ops.input=valid;ops.fail=!readFail;ops.readFail=readFail;data=nullptr;
    assert(muse::downloadGif(ops,data,bytes,muse::kGifMaxBytes));free(data);
  }
}
int main() {
  Bytes b=header();frame(b,0,0,3,2,{1,1,1,1,1,1});frame(b,0,0,2,1,{2,0},3,true);
  frame(b,2,1,1,1,{3});b.push_back(0x3b);
  downloadChecks(b);
  directChecks(b);
  unsigned count;assert(muse::validateGif(b.data(),b.size(),count)&&count==3);
  for(size_t n=0;n<b.size();++n)assert(!muse::validateGif(b.data(),n,count));
  std::vector<uint16_t> canvas(240*320),restore(canvas.size());muse::GifCanvas gif;
  assert(gif.open(b.data(),b.size(),canvas.data(),restore.data(),-1,0));
  const size_t origin=159*240+118;
  assert(gif.next(0));assert(canvas[origin]==0xf800 && canvas[origin+1]==0xf800);
  std::vector<uint16_t> scaled(240);
  assert(gif.fullscreen());gif.screenRow(0,scaled.data());
  for(auto pixel:scaled)assert(pixel==0xf800); // Small GIF fills every screen edge.
  assert(!gif.due(99) && gif.due(100));
  assert(gif.next(100));assert(canvas[origin]==0x07e0 && canvas[origin+1]==0xf800);
  gif.screenRow(0,scaled.data());assert(scaled[0]==0x07e0&&scaled[239]==0xf800);
  assert(gif.next(200));assert(canvas[origin]==0xf800 && canvas[origin+240+2]==0x001f);
  gif.screenRow(0,scaled.data());assert(scaled[0]==0xf800); // disposal 3 restores before zoom
  gif.screenRow(319,scaled.data());assert(scaled[239]==0x001f);
  assert(gif.next(300));assert(canvas[origin+240+2]==0xf800); // loop resets canvas
  // Full 256-colour palettes extend past the decoder's first 255-byte read.
  Bytes palette=header(false);palette[10]=0x87;palette.resize(13);
  for(int i=0;i<256;++i)palette.insert(palette.end(),{uint8_t(i),0,0});
  frame(palette,0,0,3,2,{1,1,1,1,1,1});palette.push_back(0x3b);
  assert(muse::validateGif(palette.data(),palette.size(),count));
  assert(gif.open(palette.data(),palette.size(),canvas.data(),restore.data(),0,0));
  assert(gif.next(0));
  Bytes suffix=b;suffix.insert(suffix.end(),{0,0,0});
  assert(muse::validateGif(suffix.data(),suffix.size(),count));
  assert(gif.open(suffix.data(),suffix.size(),canvas.data(),restore.data(),0,0));assert(gif.next(0));
  // Local 256-entry table needs the same frame-data lookahead.
  Bytes local=header(false);frame(local,0,0,3,2,{1,1,1,1,1,1},1,false,true);
  const size_t localAt=25+8+10;local[localAt-1]=0x87;
  local.erase(local.begin()+localAt,local.begin()+localAt+12);
  Bytes colors;for(int i=0;i<256;++i)colors.insert(colors.end(),{uint8_t(i),0,0});
  local.insert(local.begin()+localAt,colors.begin(),colors.end());local.push_back(0x3b);
  assert(gif.open(local.data(),local.size(),canvas.data(),restore.data(),0,0));assert(gif.next(0));
  Bytes d=header(false);frame(d,0,0,3,2,{1,1,1,1,1,1},2);frame(d,1,0,1,1,{1},1,false,true);d.push_back(0x3b);
  assert(gif.open(d.data(),d.size(),canvas.data(),restore.data(),0,0));assert(gif.next(0));assert(gif.next(100));
  assert(canvas[118]==0 && canvas[119]==0xffff); // disposal 2 and local palette
  assert(!gif.next(200) && !gif.running()); // no loop extension means one play
  d[6]=241;assert(!gif.open(d.data(),d.size(),canvas.data(),restore.data(),0,0));
  assert(!gif.open(b.data(),b.size(),canvas.data(),restore.data(),319,0));
  assert(!gif.open(b.data(),b.size(),nullptr,restore.data(),0,0));
  Bytes invalid=header();frame(invalid,2,0,2,1,{1,1});invalid.push_back(0x3b);
  assert(!muse::validateGif(invalid.data(),invalid.size(),count));
  Bytes inter=header(false);inter[8]=4;
  frame(inter,0,0,3,4,{1,1,1,3,3,3,2,2,2,1,1,1},1,false,false,true);inter.push_back(0x3b);
  assert(gif.open(inter.data(),inter.size(),canvas.data(),restore.data(),0,0));assert(gif.next(0));
  assert(canvas[118]==0xf800 && canvas[240+118]==0x07e0 && canvas[480+118]==0x001f);
  Bytes finite=header();finite[41]=1; // one extra repetition in Netscape extension
  frame(finite,0,0,3,2,{1,1,1,1,1,1});finite.push_back(0x3b);
  assert(gif.open(finite.data(),finite.size(),canvas.data(),restore.data(),0,0));
  assert(gif.next(0) && gif.next(100) && !gif.next(200));
  Bytes tooMany=header();for(int i=0;i<513;++i)frame(tooMany,0,0,1,1,{1});tooMany.push_back(0x3b);
  assert(!muse::validateGif(tooMany.data(),tooMany.size(),count));
  Bytes badDispose=header();frame(badDispose,0,0,1,1,{1},4);badDispose.push_back(0x3b);
  assert(!muse::validateGif(badDispose.data(),badDispose.size(),count));
  assert(gif.open(b.data(),b.size(),canvas.data(),restore.data(),0,0));
  assert(gif.next(UINT32_MAX-50));assert(!gif.due(48) && gif.due(49));
  assert(!strcmp(muse::turnError("NO REPLY FROM MUSE"),"Muse reply timed out"));
  assert(!strcmp(muse::turnError("remote secret"),"Muse request failed"));
  std::ifstream demoFile("docs/direct-gif-demo.gif",std::ios::binary);
  assert(demoFile.good());
  Bytes demo((std::istreambuf_iterator<char>(demoFile)),std::istreambuf_iterator<char>());
  // Feed the actual demo through the metadata-free decoder used in production.
  const char *base64="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  for(size_t i=0;i<demo.size();i+=3) {
    uint32_t v=uint32_t(demo[i])<<16;
    if(i+1<demo.size())v|=uint32_t(demo[i+1])<<8;
    if(i+2<demo.size())v|=demo[i+2];
    encoded+=base64[v>>18];encoded+=base64[(v>>12)&63];
    encoded+=i+1<demo.size()?base64[(v>>6)&63]:'=';encoded+=i+2<demo.size()?base64[v&63]:'=';
  }
  Bytes fast(muse::GifTransfer::inlineCapacity(encoded.c_str()));size_t fastBytes=0;
  assert(!muse::GifTransfer::decodeInline(encoded.c_str(),fast.data(),fast.size(),fastBytes));
  fast.resize(fastBytes);assert(fast==demo);demo.swap(fast);
  assert(muse::validateGif(demo.data(),demo.size(),count)&&count==2);
  assert(gif.open(demo.data(),demo.size(),canvas.data(),restore.data(),-1,0));
  assert(gif.next(0));assert(canvas[(128+24)*240+88+20]==0xef7e);
  gif.screenRow(122,scaled.data());assert(scaled[62]==0xef7e); // eye is enlarged at correct crop position
  gif.screenRow(160,scaled.data());assert(scaled[0]==0x8ed7&&scaled[239]==0x8ed7);

  assert(gif.next(750));assert(canvas[(128+24)*240+88+20]==0x8ed7);
  gif.screenRow(122,scaled.data());assert(scaled[62]==0x8ed7);
  // Explicit placement retains native-size output.
  assert(gif.open(demo.data(),demo.size(),canvas.data(),restore.data(),0,0));
  assert(!gif.fullscreen());assert(gif.next(0));gif.screenRow(24,scaled.data());
  assert(scaled[108]==0xef7e&&scaled[0]==0);
  // Tall source fills width; a single source pixel fills the entire display.
  Bytes tall=header(false);tall[6]=1;tall[8]=4;
  frame(tall,0,0,1,4,{1,2,3,1});tall.push_back(0x3b);
  assert(gif.open(tall.data(),tall.size(),canvas.data(),restore.data(),-1,0));assert(gif.next(0));
  gif.screenRow(0,scaled.data());for(auto pixel:scaled)assert(pixel==0x07e0);
  gif.screenRow(319,scaled.data());for(auto pixel:scaled)assert(pixel==0x001f);
  Bytes tiny=header(false);tiny[6]=tiny[8]=1;
  frame(tiny,0,0,1,1,{3});tiny.push_back(0x3b);
  assert(gif.open(tiny.data(),tiny.size(),canvas.data(),restore.data(),-1,0));assert(gif.next(0));
  for(int y:{0,159,319}) {gif.screenRow(y,scaled.data());for(auto pixel:scaled)assert(pixel==0x001f);}

  std::cout<<"GIF: actual decoder, block bounds, transparency, disposal 2/3, local palettes, loop timing and limits passed\n";
}
