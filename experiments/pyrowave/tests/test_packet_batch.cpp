#include "../receiver/packet_batch.hpp"
#include <cassert>
#include <array>
#include <random>
#include <cstdio>
int main() {
    std::mt19937 random(0x51c0de);
    for (unsigned trial=0;trial<1000;++trial) {
        const uint32_t sequence=trial%8;
        const unsigned count=2+random()%30;
        std::vector<uint32_t> words,expected_offsets(count),offsets(count,UINT32_MAX),payload{7,8,9};
        std::vector<uint8_t> classes(count);
        std::array<uint64_t,16> blocks{},empty{},coded{},expected_blocks{},expected_empty{},expected_coded{};
        for (unsigned i=0;i<count;++i) {
            classes[i]=random()%15;const uint32_t mask=random()&65535,words_in_packet=2+random()%8;
            expected_offsets[i]=uint32_t(payload.size()+words.size());
            words.push_back((sequence<<28)|(words_in_packet<<16)|mask);words.push_back(i<<8);
            for (unsigned j=2;j<words_in_packet;++j) words.push_back(random());
            expected_blocks[classes[i]]++;expected_empty[classes[i]]+=mask==0;
            expected_coded[classes[i]]+=__builtin_popcount(mask);
        }
        int decoded=0;auto expected_payload=payload;expected_payload.insert(expected_payload.end(),words.begin(),words.end());
        assert(PyroWaveV9::append_linear_packet_run(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
            sequence,offsets,payload,classes,blocks.data(),empty.data(),coded.data(),decoded));
        assert(decoded==int(count) && payload==expected_payload && offsets==expected_offsets);
        assert(blocks==expected_blocks && empty==expected_empty && coded==expected_coded);
        // Malformed final packet must be rejected before *any* output mutates.
        offsets.assign(count,UINT32_MAX);payload={7,8,9};blocks={};empty={};coded={};decoded=0;
        const auto before=payload;words.resize(words.size()-1);
        assert(!PyroWaveV9::append_linear_packet_run(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
            sequence,offsets,payload,classes,blocks.data(),empty.data(),coded.data(),decoded));
        assert(payload==before && decoded==0);
        for(auto value:offsets)assert(value==UINT32_MAX);
        for(auto value:blocks)assert(value==0);
    }
    puts("Packet batching: 1000 exact-copy/counter/reference cases and atomic malformed-tail rejection passed");
}
