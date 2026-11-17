// V9: exact-copy packet batches. No transform, quantizer or GPU ownership change.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <limits>
namespace PyroWaveV9 {
inline uint32_t load32(const uint8_t*p){uint32_t w;std::memcpy(&w,p,4);return w;}
inline bool append_linear_packet_run(const uint8_t*data,size_t size,uint32_t last_seq,
 std::vector<uint32_t>&offsets,std::vector<uint32_t>&payload,const std::vector<uint8_t>&classes,
 uint64_t*blocks,uint64_t*empty,uint64_t*coded,int&decoded){
 if(size<16 || (size&3) || (reinterpret_cast<uintptr_t>(data)&3) || last_seq==UINT32_MAX)return false;
 size_t cursor=0;uint32_t prior=0;bool first=true;unsigned count=0;
 while(cursor<size){
  if(size-cursor<8)return false;const uint32_t h=load32(data+cursor),q=load32(data+cursor+4);
  const size_t bytes=size_t((h>>16)&4095)*4;const uint32_t index=q>>8;
  if((h>>31) || ((h>>28)&7)!=(last_seq&7) || bytes<8 || bytes>size-cursor || index>=offsets.size() || index>=classes.size() || classes[index]>=15 || offsets[index]!=UINT32_MAX || (!first&&index<=prior))return false;
  prior=index;first=false;cursor+=bytes;count++;
 }
 if(cursor!=size || count<2 || payload.size()>UINT32_MAX-size/4)return false;
 const size_t base=payload.size();const auto*words=reinterpret_cast<const uint32_t*>(data);
 // One uninitialized vector append: resize+memcpy would add an unnecessary zero-fill.
 payload.insert(payload.end(),words,words+size/4);
 cursor=0;
 while(cursor<size){const uint32_t h=load32(data+cursor),q=load32(data+cursor+4),index=q>>8;const unsigned cls=classes[index];
  offsets[index]=uint32_t(base+cursor/4);decoded++;blocks[cls]++;empty[cls]+=(h&65535)==0;coded[cls]+=unsigned(__builtin_popcount(h&65535));cursor+=size_t((h>>16)&4095)*4;
 }
 return true;
}
}
