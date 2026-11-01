#include "GifTransfer.h"
#include <array>
#include <cassert>
#include <climits>
#include <iostream>
#include <string>
#include <vector>
using muse::GifTransfer;
static std::string encode(const uint8_t *data,size_t n) {
  const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for(size_t i=0;i<n;i+=3) {
    uint32_t v=uint32_t(data[i])<<16;
    if(i+1<n)v|=uint32_t(data[i+1])<<8;
    if(i+2<n)v|=data[i+2];
    out+=alphabet[v>>18];out+=alphabet[(v>>12)&63];
    out+=i+1<n?alphabet[(v>>6)&63]:'=';out+=i+2<n?alphabet[v&63]:'=';
  }return out;
}
static uint32_t crc32(const uint8_t *data,size_t n) {
  uint32_t crc=0xffffffff;
  for(size_t i=0;i<n;++i) {
    crc^=data[i];for(int j=0;j<8;++j)crc=(crc>>1)^((crc&1)?0xedb88320u:0);
  }return crc^0xffffffff;
}
int main() {
  uint32_t parsed=0;assert(GifTransfer::parseCrc("cbf43926",parsed)&&parsed==0xcbf43926);
  assert(crc32(reinterpret_cast<const uint8_t *>("123456789"),9)==parsed);
  assert(!GifTransfer::parseCrc("xyz00000",parsed));assert(!GifTransfer::parseCrc("123",parsed));
  assert(!GifTransfer::validId(""));assert(!GifTransfer::validId("a/b"));
  assert(!GifTransfer::validId(std::string(33,'a').c_str()));assert(GifTransfer::validId("test_1-2"));
  // Fast display needs only base64: no CRC or session metadata.
  std::vector<uint8_t> fast(GifTransfer::ChunkBytes),out(fast.size());
  for(size_t i=0;i<fast.size();++i)fast[i]=uint8_t(i);
  auto inlineData=encode(fast.data(),fast.size());size_t decoded=0;
  assert(GifTransfer::inlineCapacity(inlineData.c_str())==fast.size());
  assert(!GifTransfer::decodeInline(inlineData.c_str(),out.data(),out.size(),decoded));
  assert(decoded==fast.size()&&out==fast);
  assert(GifTransfer::decodeInline(inlineData.c_str(),out.data(),out.size()-1,decoded));
  for(const char *invalid:{"", "MQ", "M$==", "MR==", "MTJ=", "MQ==MQ=="})
    assert(GifTransfer::decodeInline(invalid,out.data(),out.size(),decoded));
  assert(!GifTransfer::decodeInline("MQ==",out.data(),out.size(),decoded)&&decoded==1&&out[0]=='1');
  assert(!GifTransfer::decodeInline("MTI=",out.data(),out.size(),decoded)&&decoded==2&&out[1]=='2');
  assert(!GifTransfer::inlineCapacity(std::string(8196,'A').c_str()));
  std::array<uint8_t,12> buffer;buffer.fill(0xa5);
  GifTransfer t;t.begin(buffer.data(),"test",9,0xcbf43926,-1,0);
  assert(t.same("test",9,0xcbf43926,-1));assert(!t.same("test",10,0xcbf43926,-1));
  assert(t.append("other",0,"MTIz",1));assert(t.append("test",3,"MTIz",1));
  assert(t.append("test",0,"MTI$",1));assert(t.append("test",0,"MTI= ",1));
  assert(t.append("test",0,"MR==",1));assert(t.append("test",0,"MTJ=",1)); // noncanonical pad bits
  assert(t.append("test",0,"MQ==MTIz",1));assert(t.received()==0&&buffer[0]==0xa5);
  assert(!t.append("test",0,"MTIz",1));assert(t.received()==3);
  assert(!t.append("test",0,"MTIz",2));assert(t.received()==3);
  assert(t.append("test",0,"NDU2",2));assert(t.append("test",2,"MzQ1",2));
  assert(t.ready("test",2));assert(!t.append("test",3,"NDU2Nzg5",3));
  assert(!t.ready("test",4));assert(!memcmp(buffer.data(),"123456789",9));
  assert(t.append("test",9,"MQ==",4));assert(t.append("test",SIZE_MAX,"MQ==",4));
  assert(t.release()==buffer.data());assert(!t.active());assert(t.append("test",0,"MQ==",5));
  t.begin(buffer.data(),"test",2,0,-1,UINT_MAX-10);
  assert(!t.append("test",0,"YWI=",UINT_MAX-9));assert(t.ready("test",1)); // bad checksum
  assert(!t.expired(1));assert(t.expired(30000));t.release();
  // Idle timeout is not renewed by duplicate retries.
  t.begin(buffer.data(),"test",2,0,-1,0);assert(!t.append("test",0,"YWI=",1));
  assert(!t.append("test",0,"YWI=",29999));assert(t.expired(30001));t.release();
  // Full 1 MiB payload with all byte values, maximum chunks and exact last chunk.
  std::vector<uint8_t> source(GifTransfer::MaxBytes),target(source.size());
  for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i);
  t.begin(target.data(),"large",source.size(),crc32(source.data(),source.size()),-1,0);
  for(size_t at=0;at<source.size();at+=GifTransfer::ChunkBytes) {
    const size_t n=std::min(GifTransfer::ChunkBytes,source.size()-at);
    auto encoded=encode(source.data()+at,n);
    assert(!t.append("large",at,encoded.c_str(),uint32_t(at/32)));
  }
  assert(!t.ready("large",32769));assert(source==target);t.release();
  t.begin(target.data(),"deadline",source.size(),0,-1,0);
  for(unsigned now=10000;now<300000;now+=10000)assert(!t.append("deadline",t.received(),"AAAA",now));
  assert(t.expired(300000));assert(t.append("deadline",t.received(),"AAAA",300000));t.release();
  std::cout<<"GIF transfer: strict base64, CRC32, bounds, retries, timeout, exact 1 MiB passed\n";
}
