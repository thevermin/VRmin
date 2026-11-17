// Generated CDF5/3 MSL, see port-cdf53.py. SPDX-License-Identifier: MIT
#pragma once
namespace PyroWave {
static const char dwt_cdf53_msl_source[] = R"CDF53_MSL(
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

struct Registers
{
    int2 resolution;
    float2 inv_resolution;
    int2 aligned_resolution;
};

constant bool DCShift_tmp [[function_constant(0)]];
constant bool DCShift = is_function_constant_defined(DCShift_tmp) ? DCShift_tmp : false;

static inline __attribute__((always_inline))
int2 unswizzle8x8(thread const uint& index)
{
    uint y = extract_bits(index, uint(0), uint(1));
    uint x = extract_bits(index, uint(1), uint(2));
    y |= (extract_bits(index, uint(3), uint(2)) << uint(1));
    x |= (extract_bits(index, uint(5), uint(1)) << uint(2));
    return int2(int(x), int(y));
}

static inline __attribute__((always_inline))
float2 generate_mirror_uv(thread int2& coord, constant Registers& _104)
{
    coord -= int2(coord < int2(0));
    coord += int2(1);
    int2 end_mirrored_clamp = (int2(2) * _104.aligned_resolution) - _104.resolution;
    int2 past_wrapped_coord = (coord + (int2(2) * (_104.resolution - _104.aligned_resolution))) + int2(1);
    coord = select(min(coord, _104.resolution), past_wrapped_coord, coord >= end_mirrored_clamp);
    return float2(coord) * _104.inv_resolution;
}

static inline __attribute__((always_inline))
void store_shared(thread const uint& y, thread const uint& x, thread const float2& v, threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20>& shared_block)
{
    shared_block[y][x] = v;
}

static inline __attribute__((always_inline))
void load_image_with_apron(threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20>& shared_block, constant Registers& _104, thread uint3& gl_WorkGroupID, thread uint& local_index, texture2d<float> uTexture, sampler uTextureSmplr)
{
    int2 base_coord = (int2(gl_WorkGroupID.xy) * int2(32)) - int2(4);
    uint param = local_index;
    int2 local_coord0 = int2(2) * unswizzle8x8(param);
    int2 coord0 = base_coord + local_coord0;
    int2 param_1 = coord0;
    float2 _178 = generate_mirror_uv(param_1, _104);
    float4 texels0 = uTexture.gather(uTextureSmplr, _178, int2(0), component::x).wzxy;
    int2 param_2 = coord0 + int2(16, 0);
    float2 _188 = generate_mirror_uv(param_2, _104);
    float4 texels1 = uTexture.gather(uTextureSmplr, _188, int2(0), component::x).wzxy;
    int2 param_3 = coord0 + int2(0, 16);
    float2 _197 = generate_mirror_uv(param_3, _104);
    float4 texels2 = uTexture.gather(uTextureSmplr, _197, int2(0), component::x).wzxy;
    int2 param_4 = coord0 + int2(16);
    float2 _206 = generate_mirror_uv(param_4, _104);
    float4 texels3 = uTexture.gather(uTextureSmplr, _206, int2(0), component::x).wzxy;
    if (DCShift)
    {
        texels0 -= float4(0.5);
        texels1 -= float4(0.5);
        texels2 -= float4(0.5);
        texels3 -= float4(0.5);
    }
    int local_coord0_y_half = local_coord0.y >> 1;
    uint param_5 = uint(local_coord0_y_half + 0);
    uint param_6 = uint(local_coord0.x + 0);
    float2 param_7 = texels0.xz;
    store_shared(param_5, param_6, param_7, shared_block);
    uint param_8 = uint(local_coord0_y_half + 0);
    uint param_9 = uint(local_coord0.x + 1);
    float2 param_10 = texels0.yw;
    store_shared(param_8, param_9, param_10, shared_block);
    uint param_11 = uint(local_coord0_y_half + 0);
    uint param_12 = uint(local_coord0.x + 16);
    float2 param_13 = texels1.xz;
    store_shared(param_11, param_12, param_13, shared_block);
    uint param_14 = uint(local_coord0_y_half + 0);
    uint param_15 = uint(local_coord0.x + 17);
    float2 param_16 = texels1.yw;
    store_shared(param_14, param_15, param_16, shared_block);
    uint param_17 = uint(local_coord0_y_half + 8);
    uint param_18 = uint(local_coord0.x + 0);
    float2 param_19 = texels2.xz;
    store_shared(param_17, param_18, param_19, shared_block);
    uint param_20 = uint(local_coord0_y_half + 8);
    uint param_21 = uint(local_coord0.x + 1);
    float2 param_22 = texels2.yw;
    store_shared(param_20, param_21, param_22, shared_block);
    uint param_23 = uint(local_coord0_y_half + 8);
    uint param_24 = uint(local_coord0.x + 16);
    float2 param_25 = texels3.xz;
    store_shared(param_23, param_24, param_25, shared_block);
    uint param_26 = uint(local_coord0_y_half + 8);
    uint param_27 = uint(local_coord0.x + 17);
    float2 param_28 = texels3.yw;
    store_shared(param_26, param_27, param_28, shared_block);
    int2 local_coord = int2(int(32u + (2u * (local_index % 4u))), int(2u * (local_index / 4u)));
    int2 param_29 = base_coord + local_coord;
    float2 _357 = generate_mirror_uv(param_29, _104);
    float4 texels = uTexture.gather(uTextureSmplr, _357, int2(0), component::x).wzxy;
    if (DCShift)
    {
        texels -= float4(0.5);
    }
    uint param_30 = uint(local_coord.y >> 1);
    uint param_31 = uint(local_coord.x + 0);
    float2 param_32 = texels.xz;
    store_shared(param_30, param_31, param_32, shared_block);
    uint param_33 = uint(local_coord.y >> 1);
    uint param_34 = uint(local_coord.x + 1);
    float2 param_35 = texels.yw;
    store_shared(param_33, param_34, param_35, shared_block);
    int2 local_coord_1 = int2(int(2u * (local_index % 16u)), int(32u + (2u * (local_index / 16u))));
    int2 param_36 = base_coord + local_coord_1;
    float2 _411 = generate_mirror_uv(param_36, _104);
    float4 texels_1 = uTexture.gather(uTextureSmplr, _411, int2(0), component::x).wzxy;
    if (DCShift)
    {
        texels_1 -= float4(0.5);
    }
    uint param_37 = uint(local_coord_1.y >> 1);
    uint param_38 = uint(local_coord_1.x + 0);
    float2 param_39 = texels_1.xz;
    store_shared(param_37, param_38, param_39, shared_block);
    uint param_40 = uint(local_coord_1.y >> 1);
    uint param_41 = uint(local_coord_1.x + 1);
    float2 param_42 = texels_1.yw;
    store_shared(param_40, param_41, param_42, shared_block);
    if (local_index < 16u)
    {
        int2 local_coord_2 = int2(int(32u + (2u * (local_index % 4u))), int(32u + (2u * (local_index / 4u))));
        int2 param_43 = base_coord + local_coord_2;
        float2 _469 = generate_mirror_uv(param_43, _104);
        float4 texels_2 = uTexture.gather(uTextureSmplr, _469, int2(0), component::x).wzxy;
        if (DCShift)
        {
            texels_2 -= float4(0.5);
        }
        uint param_44 = uint(local_coord_2.y >> 1);
        uint param_45 = uint(local_coord_2.x + 0);
        float2 param_46 = texels_2.xz;
        store_shared(param_44, param_45, param_46, shared_block);
        uint param_47 = uint(local_coord_2.y >> 1);
        uint param_48 = uint(local_coord_2.x + 1);
        float2 param_49 = texels_2.yw;
        store_shared(param_47, param_48, param_49, shared_block);
    }
}

static inline __attribute__((always_inline))
float2 load_shared(thread const uint& y, thread const uint& x, threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20>& shared_block)
{
    return shared_block[y][x];
}

static inline __attribute__((always_inline))
void forward_transform8x2(threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(8u * (local_index % 4u)), int(local_index / 4u));
    spvUnsafeArray<float2, 16> values;
    for (int i = 0; i < 16; i++)
    {
        uint param = uint(local_coord.y);
        uint param_1 = uint(local_coord.x + i);
        float2 v = load_shared(param, param_1, shared_block);
        values[i] = v;
    }
    // CDF5/3 (LeGall): donor predict then update, no9/7Krescale.
    for (int i_1 = 1; i_1 < 15; i_1 += 2)
    {
        values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * float(0.5));
    }
    for (int i_2 = 2; i_2 < 14; i_2 += 2)
    {
        values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * float(0.25));
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int i_5 = 2; i_5 < 6; i_5++)
    {
        float2 a = values[(2 * i_5) + 0];
        float2 b = values[(2 * i_5) + 1];
        float2 t0 = float2(a.x, b.x);
        float2 t1 = float2(a.y, b.y);
        int y_coord = (local_coord.x >> 1) + (i_5 - 2);
        uint param_2 = uint(y_coord);
        uint param_3 = uint((2 * local_coord.y) + 0);
        float2 param_4 = t0;
        store_shared(param_2, param_3, param_4, shared_block);
        uint param_5 = uint(y_coord);
        uint param_6 = uint((2 * local_coord.y) + 1);
        float2 param_7 = t1;
        store_shared(param_5, param_6, param_7, shared_block);
    }
}

static inline __attribute__((always_inline))
void forward_transform4x2(thread const bool& active_lane, thread const int& y_offset, threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(4u * (local_index % 8u)), int((local_index / 8u) + uint(y_offset)));
    spvUnsafeArray<float2, 12> values;
    if (active_lane)
    {
        for (int i = 0; i < 12; i++)
        {
            uint param = uint(local_coord.y);
            uint param_1 = uint(local_coord.x + i);
            float2 v = load_shared(param, param_1, shared_block);
            values[i] = v;
        }
        // CDF5/3 (LeGall): donor predict then update, no9/7Krescale.
        for (int i_1 = 1; i_1 < 11; i_1 += 2)
        {
            values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * float(0.5));
        }
        for (int i_2 = 2; i_2 < 10; i_2 += 2)
        {
            values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * float(0.25));
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (active_lane)
    {
        for (int i_5 = 2; i_5 < 4; i_5++)
        {
            float2 a = values[(2 * i_5) + 0];
            float2 b = values[(2 * i_5) + 1];
            float2 t0 = float2(a.x, b.x);
            float2 t1 = float2(a.y, b.y);
            int y_coord = (local_coord.x >> 1) + (i_5 - 2);
            uint param_2 = uint(y_coord);
            uint param_3 = uint((2 * local_coord.y) + 0);
            float2 param_4 = t0;
            store_shared(param_2, param_3, param_4, shared_block);
            uint param_5 = uint(y_coord);
            uint param_6 = uint((2 * local_coord.y) + 1);
            float2 param_7 = t1;
            store_shared(param_5, param_6, param_7, shared_block);
        }
    }
}

kernel void pyrowave_dwt(constant Registers& _104 [[buffer(0)]], texture2d<float> uTexture [[texture(0)]], texture2d_array<float, access::write> uOutput [[texture(1)]], sampler uTextureSmplr [[sampler(0)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint gl_SubgroupID [[simdgroup_index_in_threadgroup]], uint gl_SubgroupSize [[threads_per_simdgroup]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])
{
    threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20> shared_block;
    uint local_index = (gl_SubgroupID * gl_SubgroupSize) + gl_SubgroupInvocationID;
    load_image_with_apron(shared_block, _104, gl_WorkGroupID, local_index, uTexture, uTextureSmplr);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    forward_transform8x2(shared_block, local_index);
    bool param = local_index < 32u;
    int param_1 = 16;
    forward_transform4x2(param, param_1, shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    forward_transform8x2(shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint param_2 = local_index;
    int2 local_coord = unswizzle8x8(param_2);
    for (int y = local_coord.y; y < 16; y += 8)
    {
        int _978 = local_coord.x * 2;
        for (int x = _978; x < 32; x += 16)
        {
            uint param_3 = uint(y);
            uint param_4 = uint(x + 0);
            float2 v0 = load_shared(param_3, param_4, shared_block);
            uint param_5 = uint(y);
            uint param_6 = uint(x + 1);
            float2 v1 = load_shared(param_5, param_6, shared_block);
            int img_x = x >> 1;
            int img_y = y;
            int2 base_image_coord = (int2(gl_WorkGroupID.xy) * int2(16)) + int2(img_x, img_y);
            int3 _1027 = int3(base_image_coord, 0);
            uOutput.write(v0.xxxx, uint2(_1027.xy), uint(_1027.z));
            int3 _1034 = int3(base_image_coord, 2);
            uOutput.write(v0.yyyy, uint2(_1034.xy), uint(_1034.z));
            int3 _1041 = int3(base_image_coord, 1);
            uOutput.write(v1.xxxx, uint2(_1041.xy), uint(_1041.z));
            int3 _1048 = int3(base_image_coord, 3);
            uOutput.write(v1.yyyy, uint2(_1048.xy), uint(_1048.z));
        }
    }
}

)CDF53_MSL";
static const char idwt_cdf53_msl_source[] = R"CDF53_MSL(
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

struct Registers
{
    int2 resolution;
    float2 inv_resolution;
};

constant bool DCShift_tmp [[function_constant(0)]];
constant bool DCShift = is_function_constant_defined(DCShift_tmp) ? DCShift_tmp : false;

static inline __attribute__((always_inline))
int2 unswizzle8x8(thread const uint& index)
{
    uint y = extract_bits(index, uint(0), uint(1));
    uint x = extract_bits(index, uint(1), uint(2));
    y |= (extract_bits(index, uint(3), uint(2)) << uint(1));
    x |= (extract_bits(index, uint(5), uint(1)) << uint(2));
    return int2(int(x), int(y));
}

static inline __attribute__((always_inline))
bool2 band(thread const bool2& a, thread const bool2& b)
{
    return bool2(a.x && b.x, a.y && b.y);
}

static inline __attribute__((always_inline))
float2 generate_mirror_uv(thread int2& coord, thread const bool& even_x, thread const bool& even_y, constant Registers& _148)
{
    bool2 param = bool2(even_x, even_y);
    bool2 param_1 = coord < int2(0);
    coord -= int2(band(param, param_1));
    coord += int2(1);
    bool2 param_2 = bool2(!even_x, !even_y);
    bool2 param_3 = coord >= _148.resolution;
    coord += int2(band(param_2, param_3));
    float2 uv = float2(coord) * _148.inv_resolution;
    return uv.yx;
}

static inline __attribute__((always_inline))
void store_shared(thread const uint& y, thread const uint& x, thread const float2& v, threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20>& shared_block)
{
    shared_block[y][x] = v;
}

static inline __attribute__((always_inline))
void write_shared_4x4(thread const int2& coord, thread const float4& texels0, thread const float4& texels1, thread const float4& texels2, thread const float4& texels3, threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20>& shared_block)
{
    uint param = uint(coord.y + 0);
    uint param_1 = uint((2 * coord.x) + 0);
    float2 param_2 = float2(texels0.x, texels2.x);
    store_shared(param, param_1, param_2, shared_block);
    uint param_3 = uint(coord.y + 0);
    uint param_4 = uint((2 * coord.x) + 1);
    float2 param_5 = float2(texels1.x, texels3.x);
    store_shared(param_3, param_4, param_5, shared_block);
    uint param_6 = uint(coord.y + 0);
    uint param_7 = uint((2 * coord.x) + 2);
    float2 param_8 = float2(texels0.y, texels2.y);
    store_shared(param_6, param_7, param_8, shared_block);
    uint param_9 = uint(coord.y + 0);
    uint param_10 = uint((2 * coord.x) + 3);
    float2 param_11 = float2(texels1.y, texels3.y);
    store_shared(param_9, param_10, param_11, shared_block);
    uint param_12 = uint(coord.y + 1);
    uint param_13 = uint((2 * coord.x) + 0);
    float2 param_14 = float2(texels0.z, texels2.z);
    store_shared(param_12, param_13, param_14, shared_block);
    uint param_15 = uint(coord.y + 1);
    uint param_16 = uint((2 * coord.x) + 1);
    float2 param_17 = float2(texels1.z, texels3.z);
    store_shared(param_15, param_16, param_17, shared_block);
    uint param_18 = uint(coord.y + 1);
    uint param_19 = uint((2 * coord.x) + 2);
    float2 param_20 = float2(texels0.w, texels2.w);
    store_shared(param_18, param_19, param_20, shared_block);
    uint param_21 = uint(coord.y + 1);
    uint param_22 = uint((2 * coord.x) + 3);
    float2 param_23 = float2(texels1.w, texels3.w);
    store_shared(param_21, param_22, param_23, shared_block);
}

static inline __attribute__((always_inline))
void load_image_with_apron(threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20>& shared_block, constant Registers& _148, thread uint3& gl_WorkGroupID, thread uint& local_index, texture2d_array<float> uTexture, sampler uTextureSmplr)
{
    int2 base_coord = (int2(gl_WorkGroupID.xy) * int2(16)) - int2(2);
    uint param = local_index;
    int2 local_coord0 = int2(2) * unswizzle8x8(param);
    int2 coord0 = base_coord + local_coord0;
    int2 param_1 = coord0;
    bool param_2 = true;
    bool param_3 = true;
    float2 _353 = generate_mirror_uv(param_1, param_2, param_3, _148);
    float3 _358 = float3(_353, 0.0);
    float4 texels0 = uTexture.gather(uTextureSmplr, _358.xy, uint(rint(_358.z)), int2(0), component::x).wxzy;
    int2 param_4 = coord0;
    bool param_5 = false;
    bool param_6 = true;
    float2 _368 = generate_mirror_uv(param_4, param_5, param_6, _148);
    float3 _372 = float3(_368, 2.0);
    float4 texels1 = uTexture.gather(uTextureSmplr, _372.xy, uint(rint(_372.z)), int2(0), component::x).wxzy;
    int2 param_7 = coord0;
    bool param_8 = true;
    bool param_9 = false;
    float2 _381 = generate_mirror_uv(param_7, param_8, param_9, _148);
    float3 _385 = float3(_381, 1.0);
    float4 texels2 = uTexture.gather(uTextureSmplr, _385.xy, uint(rint(_385.z)), int2(0), component::x).wxzy;
    int2 param_10 = coord0;
    bool param_11 = false;
    bool param_12 = false;
    float2 _394 = generate_mirror_uv(param_10, param_11, param_12, _148);
    float3 _398 = float3(_394, 3.0);
    float4 texels3 = uTexture.gather(uTextureSmplr, _398.xy, uint(rint(_398.z)), int2(0), component::x).wxzy;
    int2 param_13 = local_coord0;
    float4 param_14 = texels0;
    float4 param_15 = texels1;
    float4 param_16 = texels2;
    float4 param_17 = texels3;
    write_shared_4x4(param_13, param_14, param_15, param_16, param_17, shared_block);
    int2 local_coord_horiz = int2(int(16u + (2u * (local_index % 2u))), int(2u * (local_index / 2u)));
    if (local_coord_horiz.y < 20)
    {
        int2 param_18 = base_coord + local_coord_horiz;
        bool param_19 = true;
        bool param_20 = true;
        float2 _437 = generate_mirror_uv(param_18, param_19, param_20, _148);
        float3 _440 = float3(_437, 0.0);
        texels0 = uTexture.gather(uTextureSmplr, _440.xy, uint(rint(_440.z)), int2(0), component::x).wxzy;
        int2 param_21 = base_coord + local_coord_horiz;
        bool param_22 = false;
        bool param_23 = true;
        float2 _450 = generate_mirror_uv(param_21, param_22, param_23, _148);
        float3 _453 = float3(_450, 2.0);
        texels1 = uTexture.gather(uTextureSmplr, _453.xy, uint(rint(_453.z)), int2(0), component::x).wxzy;
        int2 param_24 = base_coord + local_coord_horiz;
        bool param_25 = true;
        bool param_26 = false;
        float2 _463 = generate_mirror_uv(param_24, param_25, param_26, _148);
        float3 _466 = float3(_463, 1.0);
        texels2 = uTexture.gather(uTextureSmplr, _466.xy, uint(rint(_466.z)), int2(0), component::x).wxzy;
        int2 param_27 = base_coord + local_coord_horiz;
        bool param_28 = false;
        bool param_29 = false;
        float2 _476 = generate_mirror_uv(param_27, param_28, param_29, _148);
        float3 _479 = float3(_476, 3.0);
        texels3 = uTexture.gather(uTextureSmplr, _479.xy, uint(rint(_479.z)), int2(0), component::x).wxzy;
        int2 param_30 = local_coord_horiz;
        float4 param_31 = texels0;
        float4 param_32 = texels1;
        float4 param_33 = texels2;
        float4 param_34 = texels3;
        write_shared_4x4(param_30, param_31, param_32, param_33, param_34, shared_block);
    }
    int2 local_coord_vert = local_coord_horiz.yx;
    if (local_coord_vert.x < 16)
    {
        int2 param_35 = base_coord + local_coord_vert;
        bool param_36 = true;
        bool param_37 = true;
        float2 _508 = generate_mirror_uv(param_35, param_36, param_37, _148);
        float3 _511 = float3(_508, 0.0);
        texels0 = uTexture.gather(uTextureSmplr, _511.xy, uint(rint(_511.z)), int2(0), component::x).wxzy;
        int2 param_38 = base_coord + local_coord_vert;
        bool param_39 = false;
        bool param_40 = true;
        float2 _521 = generate_mirror_uv(param_38, param_39, param_40, _148);
        float3 _524 = float3(_521, 2.0);
        texels1 = uTexture.gather(uTextureSmplr, _524.xy, uint(rint(_524.z)), int2(0), component::x).wxzy;
        int2 param_41 = base_coord + local_coord_vert;
        bool param_42 = true;
        bool param_43 = false;
        float2 _534 = generate_mirror_uv(param_41, param_42, param_43, _148);
        float3 _537 = float3(_534, 1.0);
        texels2 = uTexture.gather(uTextureSmplr, _537.xy, uint(rint(_537.z)), int2(0), component::x).wxzy;
        int2 param_44 = base_coord + local_coord_vert;
        bool param_45 = false;
        bool param_46 = false;
        float2 _547 = generate_mirror_uv(param_44, param_45, param_46, _148);
        float3 _550 = float3(_547, 3.0);
        texels3 = uTexture.gather(uTextureSmplr, _550.xy, uint(rint(_550.z)), int2(0), component::x).wxzy;
        int2 param_47 = local_coord_vert;
        float4 param_48 = texels0;
        float4 param_49 = texels1;
        float4 param_50 = texels2;
        float4 param_51 = texels3;
        write_shared_4x4(param_47, param_48, param_49, param_50, param_51, shared_block);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
}

static inline __attribute__((always_inline))
float2 load_shared(thread const uint& y, thread const uint& x, threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20>& shared_block)
{
    return shared_block[y][x];
}

static inline __attribute__((always_inline))
void inverse_transform8x2(threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(8u * (local_index % 4u)), int(local_index / 4u));
    spvUnsafeArray<float2, 16> values;
    for (int i = 0; i < 16; i += 2)
    {
        uint param = uint(local_coord.y);
        uint param_1 = uint((local_coord.x + i) + 0);
        float2 v0 = load_shared(param, param_1, shared_block);
        uint param_2 = uint(local_coord.y);
        uint param_3 = uint((local_coord.x + i) + 1);
        float2 v1 = load_shared(param_2, param_3, shared_block);
        values[i + 0] = v0;
        values[i + 1] = v1;
    }
    // CDF5/3: undo update then predict, matching donor inverse.
    for (int i_1 = 2; i_1 < 15; i_1 += 2)
    {
        values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * float(0.25));
    }
    for (int i_2 = 3; i_2 < 14; i_2 += 2)
    {
        values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * float(0.5));
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int i_5 = 2; i_5 < 6; i_5++)
    {
        float2 a = values[(2 * i_5) + 0];
        float2 b = values[(2 * i_5) + 1];
        float2 t0 = float2(a.x, b.x);
        float2 t1 = float2(a.y, b.y);
        int y_coord = (local_coord.x >> 1) + (i_5 - 2);
        uint param_4 = uint(y_coord);
        uint param_5 = uint((2 * local_coord.y) + 0);
        float2 param_6 = t0;
        store_shared(param_4, param_5, param_6, shared_block);
        uint param_7 = uint(y_coord);
        uint param_8 = uint((2 * local_coord.y) + 1);
        float2 param_9 = t1;
        store_shared(param_7, param_8, param_9, shared_block);
    }
}

static inline __attribute__((always_inline))
void inverse_transform4x2(thread const bool& active_lane, thread const int& y_offset, threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(4u * (local_index % 8u)), int((local_index / 8u) + uint(y_offset)));
    spvUnsafeArray<float2, 12> values;
    if (active_lane)
    {
        for (int i = 0; i < 12; i += 2)
        {
            uint param = uint(local_coord.y);
            uint param_1 = uint((local_coord.x + i) + 0);
            float2 v0 = load_shared(param, param_1, shared_block);
            uint param_2 = uint(local_coord.y);
            uint param_3 = uint((local_coord.x + i) + 1);
            float2 v1 = load_shared(param_2, param_3, shared_block);
            values[i + 0] = v0;
            values[i + 1] = v1;
        }
        // CDF5/3: undo update then predict, matching donor inverse.
        for (int i_1 = 2; i_1 < 11; i_1 += 2)
        {
            values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * float(0.25));
        }
        for (int i_2 = 3; i_2 < 10; i_2 += 2)
        {
            values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * float(0.5));
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (active_lane)
    {
        for (int i_5 = 2; i_5 < 4; i_5++)
        {
            float2 a = values[(2 * i_5) + 0];
            float2 b = values[(2 * i_5) + 1];
            float2 t0 = float2(a.x, b.x);
            float2 t1 = float2(a.y, b.y);
            int y_coord = (local_coord.x >> 1) + (i_5 - 2);
            uint param_4 = uint(y_coord);
            uint param_5 = uint((2 * local_coord.y) + 0);
            float2 param_6 = t0;
            store_shared(param_4, param_5, param_6, shared_block);
            uint param_7 = uint(y_coord);
            uint param_8 = uint((2 * local_coord.y) + 1);
            float2 param_9 = t1;
            store_shared(param_7, param_8, param_9, shared_block);
        }
    }
}

kernel void pyrowave_idwt(constant Registers& _148 [[buffer(0)]], texture2d_array<float> uTexture [[texture(0)]], texture2d<float, access::write> uOutput [[texture(1)]], sampler uTextureSmplr [[sampler(0)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]])
{
    threadgroup spvUnsafeArray<spvUnsafeArray<float2, 41>, 20> shared_block;
    uint local_index = gl_LocalInvocationIndex;
    load_image_with_apron(shared_block, _148, gl_WorkGroupID, local_index, uTexture, uTextureSmplr);
    inverse_transform8x2(shared_block, local_index);
    bool param = local_index < 32u;
    int param_1 = 16;
    inverse_transform4x2(param, param_1, shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    inverse_transform8x2(shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint param_2 = local_index;
    int2 local_coord = unswizzle8x8(param_2);
    for (int y = local_coord.y; y < 16; y += 8)
    {
        for (int x = local_coord.x; x < 32; x += 8)
        {
            uint param_3 = uint(y);
            uint param_4 = uint(x);
            float2 v = load_shared(param_3, param_4, shared_block);
            if (DCShift)
            {
                v += float2(0.5);
            }
            uOutput.write(v.xxxx, uint2((int2((2 * y) + 0, x) + (int2(32) * int2(gl_WorkGroupID.yx)))));
            uOutput.write(v.yyyy, uint2((int2((2 * y) + 1, x) + (int2(32) * int2(gl_WorkGroupID.yx)))));
        }
    }
}

)CDF53_MSL";
static const char dwt_cdf53_fp16_storage_msl_source[] = R"CDF53_MSL(
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

struct Registers
{
    int2 resolution;
    float2 inv_resolution;
    int2 aligned_resolution;
};

constant bool DCShift_tmp [[function_constant(0)]];
constant bool DCShift = is_function_constant_defined(DCShift_tmp) ? DCShift_tmp : false;

static inline __attribute__((always_inline))
int2 unswizzle8x8(thread const uint& index)
{
    uint y = extract_bits(index, uint(0), uint(1));
    uint x = extract_bits(index, uint(1), uint(2));
    y |= (extract_bits(index, uint(3), uint(2)) << uint(1));
    x |= (extract_bits(index, uint(5), uint(1)) << uint(2));
    return int2(int(x), int(y));
}

static inline __attribute__((always_inline))
float2 generate_mirror_uv(thread int2& coord, constant Registers& _108)
{
    coord -= int2(coord < int2(0));
    coord += int2(1);
    int2 end_mirrored_clamp = (int2(2) * _108.aligned_resolution) - _108.resolution;
    int2 past_wrapped_coord = (coord + (int2(2) * (_108.resolution - _108.aligned_resolution))) + int2(1);
    coord = select(min(coord, _108.resolution), past_wrapped_coord, coord >= end_mirrored_clamp);
    return float2(coord) * _108.inv_resolution;
}

static inline __attribute__((always_inline))
void store_shared(thread const uint& y, thread const uint& x, thread const float2& v, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block)
{
    shared_block[y][x] = half2(v);
}

static inline __attribute__((always_inline))
void load_image_with_apron(threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, constant Registers& _108, thread uint3& gl_WorkGroupID, thread uint& local_index, texture2d<float> uTexture, sampler uTextureSmplr)
{
    int2 base_coord = (int2(gl_WorkGroupID.xy) * int2(32)) - int2(4);
    uint param = local_index;
    int2 local_coord0 = int2(2) * unswizzle8x8(param);
    int2 coord0 = base_coord + local_coord0;
    int2 param_1 = coord0;
    float2 _182 = generate_mirror_uv(param_1, _108);
    float4 texels0 = uTexture.gather(uTextureSmplr, _182, int2(0), component::x).wzxy;
    int2 param_2 = coord0 + int2(16, 0);
    float2 _192 = generate_mirror_uv(param_2, _108);
    float4 texels1 = uTexture.gather(uTextureSmplr, _192, int2(0), component::x).wzxy;
    int2 param_3 = coord0 + int2(0, 16);
    float2 _201 = generate_mirror_uv(param_3, _108);
    float4 texels2 = uTexture.gather(uTextureSmplr, _201, int2(0), component::x).wzxy;
    int2 param_4 = coord0 + int2(16);
    float2 _210 = generate_mirror_uv(param_4, _108);
    float4 texels3 = uTexture.gather(uTextureSmplr, _210, int2(0), component::x).wzxy;
    if (DCShift)
    {
        texels0 -= float4(0.5);
        texels1 -= float4(0.5);
        texels2 -= float4(0.5);
        texels3 -= float4(0.5);
    }
    int local_coord0_y_half = local_coord0.y >> 1;
    uint param_5 = uint(local_coord0_y_half + 0);
    uint param_6 = uint(local_coord0.x + 0);
    float2 param_7 = texels0.xz;
    store_shared(param_5, param_6, param_7, shared_block);
    uint param_8 = uint(local_coord0_y_half + 0);
    uint param_9 = uint(local_coord0.x + 1);
    float2 param_10 = texels0.yw;
    store_shared(param_8, param_9, param_10, shared_block);
    uint param_11 = uint(local_coord0_y_half + 0);
    uint param_12 = uint(local_coord0.x + 16);
    float2 param_13 = texels1.xz;
    store_shared(param_11, param_12, param_13, shared_block);
    uint param_14 = uint(local_coord0_y_half + 0);
    uint param_15 = uint(local_coord0.x + 17);
    float2 param_16 = texels1.yw;
    store_shared(param_14, param_15, param_16, shared_block);
    uint param_17 = uint(local_coord0_y_half + 8);
    uint param_18 = uint(local_coord0.x + 0);
    float2 param_19 = texels2.xz;
    store_shared(param_17, param_18, param_19, shared_block);
    uint param_20 = uint(local_coord0_y_half + 8);
    uint param_21 = uint(local_coord0.x + 1);
    float2 param_22 = texels2.yw;
    store_shared(param_20, param_21, param_22, shared_block);
    uint param_23 = uint(local_coord0_y_half + 8);
    uint param_24 = uint(local_coord0.x + 16);
    float2 param_25 = texels3.xz;
    store_shared(param_23, param_24, param_25, shared_block);
    uint param_26 = uint(local_coord0_y_half + 8);
    uint param_27 = uint(local_coord0.x + 17);
    float2 param_28 = texels3.yw;
    store_shared(param_26, param_27, param_28, shared_block);
    int2 local_coord = int2(int(32u + (2u * (local_index % 4u))), int(2u * (local_index / 4u)));
    int2 param_29 = base_coord + local_coord;
    float2 _361 = generate_mirror_uv(param_29, _108);
    float4 texels = uTexture.gather(uTextureSmplr, _361, int2(0), component::x).wzxy;
    if (DCShift)
    {
        texels -= float4(0.5);
    }
    uint param_30 = uint(local_coord.y >> 1);
    uint param_31 = uint(local_coord.x + 0);
    float2 param_32 = texels.xz;
    store_shared(param_30, param_31, param_32, shared_block);
    uint param_33 = uint(local_coord.y >> 1);
    uint param_34 = uint(local_coord.x + 1);
    float2 param_35 = texels.yw;
    store_shared(param_33, param_34, param_35, shared_block);
    int2 local_coord_1 = int2(int(2u * (local_index % 16u)), int(32u + (2u * (local_index / 16u))));
    int2 param_36 = base_coord + local_coord_1;
    float2 _415 = generate_mirror_uv(param_36, _108);
    float4 texels_1 = uTexture.gather(uTextureSmplr, _415, int2(0), component::x).wzxy;
    if (DCShift)
    {
        texels_1 -= float4(0.5);
    }
    uint param_37 = uint(local_coord_1.y >> 1);
    uint param_38 = uint(local_coord_1.x + 0);
    float2 param_39 = texels_1.xz;
    store_shared(param_37, param_38, param_39, shared_block);
    uint param_40 = uint(local_coord_1.y >> 1);
    uint param_41 = uint(local_coord_1.x + 1);
    float2 param_42 = texels_1.yw;
    store_shared(param_40, param_41, param_42, shared_block);
    if (local_index < 16u)
    {
        int2 local_coord_2 = int2(int(32u + (2u * (local_index % 4u))), int(32u + (2u * (local_index / 4u))));
        int2 param_43 = base_coord + local_coord_2;
        float2 _473 = generate_mirror_uv(param_43, _108);
        float4 texels_2 = uTexture.gather(uTextureSmplr, _473, int2(0), component::x).wzxy;
        if (DCShift)
        {
            texels_2 -= float4(0.5);
        }
        uint param_44 = uint(local_coord_2.y >> 1);
        uint param_45 = uint(local_coord_2.x + 0);
        float2 param_46 = texels_2.xz;
        store_shared(param_44, param_45, param_46, shared_block);
        uint param_47 = uint(local_coord_2.y >> 1);
        uint param_48 = uint(local_coord_2.x + 1);
        float2 param_49 = texels_2.yw;
        store_shared(param_47, param_48, param_49, shared_block);
    }
}

static inline __attribute__((always_inline))
float2 load_shared(thread const uint& y, thread const uint& x, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block)
{
    return float2(shared_block[y][x]);
}

static inline __attribute__((always_inline))
void forward_transform8x2(threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(8u * (local_index % 4u)), int(local_index / 4u));
    spvUnsafeArray<float2, 16> values;
    for (int i = 0; i < 16; i++)
    {
        uint param = uint(local_coord.y);
        uint param_1 = uint(local_coord.x + i);
        float2 v = load_shared(param, param_1, shared_block);
        values[i] = v;
    }
    // CDF5/3 (LeGall): donor predict then update, no9/7Krescale.
    for (int i_1 = 1; i_1 < 15; i_1 += 2)
    {
        values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * float(0.5));
    }
    for (int i_2 = 2; i_2 < 14; i_2 += 2)
    {
        values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * float(0.25));
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int i_5 = 2; i_5 < 6; i_5++)
    {
        float2 a = values[(2 * i_5) + 0];
        float2 b = values[(2 * i_5) + 1];
        float2 t0 = float2(a.x, b.x);
        float2 t1 = float2(a.y, b.y);
        int y_coord = (local_coord.x >> 1) + (i_5 - 2);
        uint param_2 = uint(y_coord);
        uint param_3 = uint((2 * local_coord.y) + 0);
        float2 param_4 = t0;
        store_shared(param_2, param_3, param_4, shared_block);
        uint param_5 = uint(y_coord);
        uint param_6 = uint((2 * local_coord.y) + 1);
        float2 param_7 = t1;
        store_shared(param_5, param_6, param_7, shared_block);
    }
}

static inline __attribute__((always_inline))
void forward_transform4x2(thread const bool& active_lane, thread const int& y_offset, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(4u * (local_index % 8u)), int((local_index / 8u) + uint(y_offset)));
    spvUnsafeArray<float2, 12> values;
    if (active_lane)
    {
        for (int i = 0; i < 12; i++)
        {
            uint param = uint(local_coord.y);
            uint param_1 = uint(local_coord.x + i);
            float2 v = load_shared(param, param_1, shared_block);
            values[i] = v;
        }
        // CDF5/3 (LeGall): donor predict then update, no9/7Krescale.
        for (int i_1 = 1; i_1 < 11; i_1 += 2)
        {
            values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * float(0.5));
        }
        for (int i_2 = 2; i_2 < 10; i_2 += 2)
        {
            values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * float(0.25));
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (active_lane)
    {
        for (int i_5 = 2; i_5 < 4; i_5++)
        {
            float2 a = values[(2 * i_5) + 0];
            float2 b = values[(2 * i_5) + 1];
            float2 t0 = float2(a.x, b.x);
            float2 t1 = float2(a.y, b.y);
            int y_coord = (local_coord.x >> 1) + (i_5 - 2);
            uint param_2 = uint(y_coord);
            uint param_3 = uint((2 * local_coord.y) + 0);
            float2 param_4 = t0;
            store_shared(param_2, param_3, param_4, shared_block);
            uint param_5 = uint(y_coord);
            uint param_6 = uint((2 * local_coord.y) + 1);
            float2 param_7 = t1;
            store_shared(param_5, param_6, param_7, shared_block);
        }
    }
}

kernel void pyrowave_dwt(constant Registers& _108 [[buffer(0)]], texture2d<float> uTexture [[texture(0)]], texture2d_array<float, access::write> uOutput [[texture(1)]], sampler uTextureSmplr [[sampler(0)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint gl_SubgroupID [[simdgroup_index_in_threadgroup]], uint gl_SubgroupSize [[threads_per_simdgroup]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])
{
    threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20> shared_block;
    uint local_index = (gl_SubgroupID * gl_SubgroupSize) + gl_SubgroupInvocationID;
    load_image_with_apron(shared_block, _108, gl_WorkGroupID, local_index, uTexture, uTextureSmplr);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    forward_transform8x2(shared_block, local_index);
    bool param = local_index < 32u;
    int param_1 = 16;
    forward_transform4x2(param, param_1, shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    forward_transform8x2(shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint param_2 = local_index;
    int2 local_coord = unswizzle8x8(param_2);
    for (int y = local_coord.y; y < 16; y += 8)
    {
        int _982 = local_coord.x * 2;
        for (int x = _982; x < 32; x += 16)
        {
            uint param_3 = uint(y);
            uint param_4 = uint(x + 0);
            float2 v0 = load_shared(param_3, param_4, shared_block);
            uint param_5 = uint(y);
            uint param_6 = uint(x + 1);
            float2 v1 = load_shared(param_5, param_6, shared_block);
            int img_x = x >> 1;
            int img_y = y;
            int2 base_image_coord = (int2(gl_WorkGroupID.xy) * int2(16)) + int2(img_x, img_y);
            int3 _1031 = int3(base_image_coord, 0);
            uOutput.write(v0.xxxx, uint2(_1031.xy), uint(_1031.z));
            int3 _1038 = int3(base_image_coord, 2);
            uOutput.write(v0.yyyy, uint2(_1038.xy), uint(_1038.z));
            int3 _1045 = int3(base_image_coord, 1);
            uOutput.write(v1.xxxx, uint2(_1045.xy), uint(_1045.z));
            int3 _1052 = int3(base_image_coord, 3);
            uOutput.write(v1.yyyy, uint2(_1052.xy), uint(_1052.z));
        }
    }
}

)CDF53_MSL";
static const char idwt_cdf53_fp16_storage_msl_source[] = R"CDF53_MSL(
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

struct Registers
{
    int2 resolution;
    float2 inv_resolution;
};

constant bool DCShift_tmp [[function_constant(0)]];
constant bool DCShift = is_function_constant_defined(DCShift_tmp) ? DCShift_tmp : false;

static inline __attribute__((always_inline))
int2 unswizzle8x8(thread const uint& index)
{
    uint y = extract_bits(index, uint(0), uint(1));
    uint x = extract_bits(index, uint(1), uint(2));
    y |= (extract_bits(index, uint(3), uint(2)) << uint(1));
    x |= (extract_bits(index, uint(5), uint(1)) << uint(2));
    return int2(int(x), int(y));
}

static inline __attribute__((always_inline))
bool2 band(thread const bool2& a, thread const bool2& b)
{
    return bool2(a.x && b.x, a.y && b.y);
}

static inline __attribute__((always_inline))
float2 generate_mirror_uv(thread int2& coord, thread const bool& even_x, thread const bool& even_y, constant Registers& _152)
{
    bool2 param = bool2(even_x, even_y);
    bool2 param_1 = coord < int2(0);
    coord -= int2(band(param, param_1));
    coord += int2(1);
    bool2 param_2 = bool2(!even_x, !even_y);
    bool2 param_3 = coord >= _152.resolution;
    coord += int2(band(param_2, param_3));
    float2 uv = float2(coord) * _152.inv_resolution;
    return uv.yx;
}

static inline __attribute__((always_inline))
void store_shared(thread const uint& y, thread const uint& x, thread const float2& v, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block)
{
    shared_block[y][x] = half2(v);
}

static inline __attribute__((always_inline))
void write_shared_4x4(thread const int2& coord, thread const float4& texels0, thread const float4& texels1, thread const float4& texels2, thread const float4& texels3, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block)
{
    uint param = uint(coord.y + 0);
    uint param_1 = uint((2 * coord.x) + 0);
    float2 param_2 = float2(texels0.x, texels2.x);
    store_shared(param, param_1, param_2, shared_block);
    uint param_3 = uint(coord.y + 0);
    uint param_4 = uint((2 * coord.x) + 1);
    float2 param_5 = float2(texels1.x, texels3.x);
    store_shared(param_3, param_4, param_5, shared_block);
    uint param_6 = uint(coord.y + 0);
    uint param_7 = uint((2 * coord.x) + 2);
    float2 param_8 = float2(texels0.y, texels2.y);
    store_shared(param_6, param_7, param_8, shared_block);
    uint param_9 = uint(coord.y + 0);
    uint param_10 = uint((2 * coord.x) + 3);
    float2 param_11 = float2(texels1.y, texels3.y);
    store_shared(param_9, param_10, param_11, shared_block);
    uint param_12 = uint(coord.y + 1);
    uint param_13 = uint((2 * coord.x) + 0);
    float2 param_14 = float2(texels0.z, texels2.z);
    store_shared(param_12, param_13, param_14, shared_block);
    uint param_15 = uint(coord.y + 1);
    uint param_16 = uint((2 * coord.x) + 1);
    float2 param_17 = float2(texels1.z, texels3.z);
    store_shared(param_15, param_16, param_17, shared_block);
    uint param_18 = uint(coord.y + 1);
    uint param_19 = uint((2 * coord.x) + 2);
    float2 param_20 = float2(texels0.w, texels2.w);
    store_shared(param_18, param_19, param_20, shared_block);
    uint param_21 = uint(coord.y + 1);
    uint param_22 = uint((2 * coord.x) + 3);
    float2 param_23 = float2(texels1.w, texels3.w);
    store_shared(param_21, param_22, param_23, shared_block);
}

static inline __attribute__((always_inline))
void load_image_with_apron(threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, constant Registers& _152, thread uint3& gl_WorkGroupID, thread uint& local_index, texture2d_array<float> uTexture, sampler uTextureSmplr)
{
    int2 base_coord = (int2(gl_WorkGroupID.xy) * int2(16)) - int2(2);
    uint param = local_index;
    int2 local_coord0 = int2(2) * unswizzle8x8(param);
    int2 coord0 = base_coord + local_coord0;
    int2 param_1 = coord0;
    bool param_2 = true;
    bool param_3 = true;
    float2 _357 = generate_mirror_uv(param_1, param_2, param_3, _152);
    float3 _362 = float3(_357, 0.0);
    float4 texels0 = uTexture.gather(uTextureSmplr, _362.xy, uint(rint(_362.z)), int2(0), component::x).wxzy;
    int2 param_4 = coord0;
    bool param_5 = false;
    bool param_6 = true;
    float2 _372 = generate_mirror_uv(param_4, param_5, param_6, _152);
    float3 _376 = float3(_372, 2.0);
    float4 texels1 = uTexture.gather(uTextureSmplr, _376.xy, uint(rint(_376.z)), int2(0), component::x).wxzy;
    int2 param_7 = coord0;
    bool param_8 = true;
    bool param_9 = false;
    float2 _385 = generate_mirror_uv(param_7, param_8, param_9, _152);
    float3 _389 = float3(_385, 1.0);
    float4 texels2 = uTexture.gather(uTextureSmplr, _389.xy, uint(rint(_389.z)), int2(0), component::x).wxzy;
    int2 param_10 = coord0;
    bool param_11 = false;
    bool param_12 = false;
    float2 _398 = generate_mirror_uv(param_10, param_11, param_12, _152);
    float3 _402 = float3(_398, 3.0);
    float4 texels3 = uTexture.gather(uTextureSmplr, _402.xy, uint(rint(_402.z)), int2(0), component::x).wxzy;
    int2 param_13 = local_coord0;
    float4 param_14 = texels0;
    float4 param_15 = texels1;
    float4 param_16 = texels2;
    float4 param_17 = texels3;
    write_shared_4x4(param_13, param_14, param_15, param_16, param_17, shared_block);
    int2 local_coord_horiz = int2(int(16u + (2u * (local_index % 2u))), int(2u * (local_index / 2u)));
    if (local_coord_horiz.y < 20)
    {
        int2 param_18 = base_coord + local_coord_horiz;
        bool param_19 = true;
        bool param_20 = true;
        float2 _441 = generate_mirror_uv(param_18, param_19, param_20, _152);
        float3 _444 = float3(_441, 0.0);
        texels0 = uTexture.gather(uTextureSmplr, _444.xy, uint(rint(_444.z)), int2(0), component::x).wxzy;
        int2 param_21 = base_coord + local_coord_horiz;
        bool param_22 = false;
        bool param_23 = true;
        float2 _454 = generate_mirror_uv(param_21, param_22, param_23, _152);
        float3 _457 = float3(_454, 2.0);
        texels1 = uTexture.gather(uTextureSmplr, _457.xy, uint(rint(_457.z)), int2(0), component::x).wxzy;
        int2 param_24 = base_coord + local_coord_horiz;
        bool param_25 = true;
        bool param_26 = false;
        float2 _467 = generate_mirror_uv(param_24, param_25, param_26, _152);
        float3 _470 = float3(_467, 1.0);
        texels2 = uTexture.gather(uTextureSmplr, _470.xy, uint(rint(_470.z)), int2(0), component::x).wxzy;
        int2 param_27 = base_coord + local_coord_horiz;
        bool param_28 = false;
        bool param_29 = false;
        float2 _480 = generate_mirror_uv(param_27, param_28, param_29, _152);
        float3 _483 = float3(_480, 3.0);
        texels3 = uTexture.gather(uTextureSmplr, _483.xy, uint(rint(_483.z)), int2(0), component::x).wxzy;
        int2 param_30 = local_coord_horiz;
        float4 param_31 = texels0;
        float4 param_32 = texels1;
        float4 param_33 = texels2;
        float4 param_34 = texels3;
        write_shared_4x4(param_30, param_31, param_32, param_33, param_34, shared_block);
    }
    int2 local_coord_vert = local_coord_horiz.yx;
    if (local_coord_vert.x < 16)
    {
        int2 param_35 = base_coord + local_coord_vert;
        bool param_36 = true;
        bool param_37 = true;
        float2 _512 = generate_mirror_uv(param_35, param_36, param_37, _152);
        float3 _515 = float3(_512, 0.0);
        texels0 = uTexture.gather(uTextureSmplr, _515.xy, uint(rint(_515.z)), int2(0), component::x).wxzy;
        int2 param_38 = base_coord + local_coord_vert;
        bool param_39 = false;
        bool param_40 = true;
        float2 _525 = generate_mirror_uv(param_38, param_39, param_40, _152);
        float3 _528 = float3(_525, 2.0);
        texels1 = uTexture.gather(uTextureSmplr, _528.xy, uint(rint(_528.z)), int2(0), component::x).wxzy;
        int2 param_41 = base_coord + local_coord_vert;
        bool param_42 = true;
        bool param_43 = false;
        float2 _538 = generate_mirror_uv(param_41, param_42, param_43, _152);
        float3 _541 = float3(_538, 1.0);
        texels2 = uTexture.gather(uTextureSmplr, _541.xy, uint(rint(_541.z)), int2(0), component::x).wxzy;
        int2 param_44 = base_coord + local_coord_vert;
        bool param_45 = false;
        bool param_46 = false;
        float2 _551 = generate_mirror_uv(param_44, param_45, param_46, _152);
        float3 _554 = float3(_551, 3.0);
        texels3 = uTexture.gather(uTextureSmplr, _554.xy, uint(rint(_554.z)), int2(0), component::x).wxzy;
        int2 param_47 = local_coord_vert;
        float4 param_48 = texels0;
        float4 param_49 = texels1;
        float4 param_50 = texels2;
        float4 param_51 = texels3;
        write_shared_4x4(param_47, param_48, param_49, param_50, param_51, shared_block);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
}

static inline __attribute__((always_inline))
float2 load_shared(thread const uint& y, thread const uint& x, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block)
{
    return float2(shared_block[y][x]);
}

static inline __attribute__((always_inline))
void inverse_transform8x2(threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(8u * (local_index % 4u)), int(local_index / 4u));
    spvUnsafeArray<float2, 16> values;
    for (int i = 0; i < 16; i += 2)
    {
        uint param = uint(local_coord.y);
        uint param_1 = uint((local_coord.x + i) + 0);
        float2 v0 = load_shared(param, param_1, shared_block);
        uint param_2 = uint(local_coord.y);
        uint param_3 = uint((local_coord.x + i) + 1);
        float2 v1 = load_shared(param_2, param_3, shared_block);
        values[i + 0] = v0;
        values[i + 1] = v1;
    }
    // CDF5/3: undo update then predict, matching donor inverse.
    for (int i_1 = 2; i_1 < 15; i_1 += 2)
    {
        values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * float(0.25));
    }
    for (int i_2 = 3; i_2 < 14; i_2 += 2)
    {
        values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * float(0.5));
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int i_5 = 2; i_5 < 6; i_5++)
    {
        float2 a = values[(2 * i_5) + 0];
        float2 b = values[(2 * i_5) + 1];
        float2 t0 = float2(a.x, b.x);
        float2 t1 = float2(a.y, b.y);
        int y_coord = (local_coord.x >> 1) + (i_5 - 2);
        uint param_4 = uint(y_coord);
        uint param_5 = uint((2 * local_coord.y) + 0);
        float2 param_6 = t0;
        store_shared(param_4, param_5, param_6, shared_block);
        uint param_7 = uint(y_coord);
        uint param_8 = uint((2 * local_coord.y) + 1);
        float2 param_9 = t1;
        store_shared(param_7, param_8, param_9, shared_block);
    }
}

static inline __attribute__((always_inline))
void inverse_transform4x2(thread const bool& active_lane, thread const int& y_offset, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(4u * (local_index % 8u)), int((local_index / 8u) + uint(y_offset)));
    spvUnsafeArray<float2, 12> values;
    if (active_lane)
    {
        for (int i = 0; i < 12; i += 2)
        {
            uint param = uint(local_coord.y);
            uint param_1 = uint((local_coord.x + i) + 0);
            float2 v0 = load_shared(param, param_1, shared_block);
            uint param_2 = uint(local_coord.y);
            uint param_3 = uint((local_coord.x + i) + 1);
            float2 v1 = load_shared(param_2, param_3, shared_block);
            values[i + 0] = v0;
            values[i + 1] = v1;
        }
        // CDF5/3: undo update then predict, matching donor inverse.
        for (int i_1 = 2; i_1 < 11; i_1 += 2)
        {
            values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * float(0.25));
        }
        for (int i_2 = 3; i_2 < 10; i_2 += 2)
        {
            values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * float(0.5));
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (active_lane)
    {
        for (int i_5 = 2; i_5 < 4; i_5++)
        {
            float2 a = values[(2 * i_5) + 0];
            float2 b = values[(2 * i_5) + 1];
            float2 t0 = float2(a.x, b.x);
            float2 t1 = float2(a.y, b.y);
            int y_coord = (local_coord.x >> 1) + (i_5 - 2);
            uint param_4 = uint(y_coord);
            uint param_5 = uint((2 * local_coord.y) + 0);
            float2 param_6 = t0;
            store_shared(param_4, param_5, param_6, shared_block);
            uint param_7 = uint(y_coord);
            uint param_8 = uint((2 * local_coord.y) + 1);
            float2 param_9 = t1;
            store_shared(param_7, param_8, param_9, shared_block);
        }
    }
}

kernel void pyrowave_idwt(constant Registers& _152 [[buffer(0)]], texture2d_array<float> uTexture [[texture(0)]], texture2d<float, access::write> uOutput [[texture(1)]], sampler uTextureSmplr [[sampler(0)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]])
{
    threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20> shared_block;
    uint local_index = gl_LocalInvocationIndex;
    load_image_with_apron(shared_block, _152, gl_WorkGroupID, local_index, uTexture, uTextureSmplr);
    inverse_transform8x2(shared_block, local_index);
    bool param = local_index < 32u;
    int param_1 = 16;
    inverse_transform4x2(param, param_1, shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    inverse_transform8x2(shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint param_2 = local_index;
    int2 local_coord = unswizzle8x8(param_2);
    for (int y = local_coord.y; y < 16; y += 8)
    {
        for (int x = local_coord.x; x < 32; x += 8)
        {
            uint param_3 = uint(y);
            uint param_4 = uint(x);
            float2 v = load_shared(param_3, param_4, shared_block);
            if (DCShift)
            {
                v += float2(0.5);
            }
            uOutput.write(v.xxxx, uint2((int2((2 * y) + 0, x) + (int2(32) * int2(gl_WorkGroupID.yx)))));
            uOutput.write(v.yyyy, uint2((int2((2 * y) + 1, x) + (int2(32) * int2(gl_WorkGroupID.yx)))));
        }
    }
}

)CDF53_MSL";
static const char dwt_cdf53_fp16_msl_source[] = R"CDF53_MSL(
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

struct Registers
{
    int2 resolution;
    float2 inv_resolution;
    int2 aligned_resolution;
};

constant bool DCShift_tmp [[function_constant(0)]];
constant bool DCShift = is_function_constant_defined(DCShift_tmp) ? DCShift_tmp : false;

static inline __attribute__((always_inline))
int2 unswizzle8x8(thread const uint& index)
{
    uint y = extract_bits(index, uint(0), uint(1));
    uint x = extract_bits(index, uint(1), uint(2));
    y |= (extract_bits(index, uint(3), uint(2)) << uint(1));
    x |= (extract_bits(index, uint(5), uint(1)) << uint(2));
    return int2(int(x), int(y));
}

static inline __attribute__((always_inline))
float2 generate_mirror_uv(thread int2& coord, constant Registers& _106)
{
    coord -= int2(coord < int2(0));
    coord += int2(1);
    int2 end_mirrored_clamp = (int2(2) * _106.aligned_resolution) - _106.resolution;
    int2 past_wrapped_coord = (coord + (int2(2) * (_106.resolution - _106.aligned_resolution))) + int2(1);
    coord = select(min(coord, _106.resolution), past_wrapped_coord, coord >= end_mirrored_clamp);
    return float2(coord) * _106.inv_resolution;
}

static inline __attribute__((always_inline))
void store_shared(thread const uint& y, thread const uint& x, thread const half2& v, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block)
{
    shared_block[y][x] = v;
}

static inline __attribute__((always_inline))
void load_image_with_apron(threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, constant Registers& _106, thread uint3& gl_WorkGroupID, thread uint& local_index, texture2d<float> uTexture, sampler uTextureSmplr)
{
    int2 base_coord = (int2(gl_WorkGroupID.xy) * int2(32)) - int2(4);
    uint param = local_index;
    int2 local_coord0 = int2(2) * unswizzle8x8(param);
    int2 coord0 = base_coord + local_coord0;
    int2 param_1 = coord0;
    float2 _180 = generate_mirror_uv(param_1, _106);
    half4 texels0 = half4(uTexture.gather(uTextureSmplr, _180, int2(0), component::x)).wzxy;
    int2 param_2 = coord0 + int2(16, 0);
    float2 _192 = generate_mirror_uv(param_2, _106);
    half4 texels1 = half4(uTexture.gather(uTextureSmplr, _192, int2(0), component::x)).wzxy;
    int2 param_3 = coord0 + int2(0, 16);
    float2 _202 = generate_mirror_uv(param_3, _106);
    half4 texels2 = half4(uTexture.gather(uTextureSmplr, _202, int2(0), component::x)).wzxy;
    int2 param_4 = coord0 + int2(16);
    float2 _212 = generate_mirror_uv(param_4, _106);
    half4 texels3 = half4(uTexture.gather(uTextureSmplr, _212, int2(0), component::x)).wzxy;
    if (DCShift)
    {
        texels0 -= half4(half(0.5));
        texels1 -= half4(half(0.5));
        texels2 -= half4(half(0.5));
        texels3 -= half4(half(0.5));
    }
    int local_coord0_y_half = local_coord0.y >> 1;
    uint param_5 = uint(local_coord0_y_half + 0);
    uint param_6 = uint(local_coord0.x + 0);
    half2 param_7 = texels0.xz;
    store_shared(param_5, param_6, param_7, shared_block);
    uint param_8 = uint(local_coord0_y_half + 0);
    uint param_9 = uint(local_coord0.x + 1);
    half2 param_10 = texels0.yw;
    store_shared(param_8, param_9, param_10, shared_block);
    uint param_11 = uint(local_coord0_y_half + 0);
    uint param_12 = uint(local_coord0.x + 16);
    half2 param_13 = texels1.xz;
    store_shared(param_11, param_12, param_13, shared_block);
    uint param_14 = uint(local_coord0_y_half + 0);
    uint param_15 = uint(local_coord0.x + 17);
    half2 param_16 = texels1.yw;
    store_shared(param_14, param_15, param_16, shared_block);
    uint param_17 = uint(local_coord0_y_half + 8);
    uint param_18 = uint(local_coord0.x + 0);
    half2 param_19 = texels2.xz;
    store_shared(param_17, param_18, param_19, shared_block);
    uint param_20 = uint(local_coord0_y_half + 8);
    uint param_21 = uint(local_coord0.x + 1);
    half2 param_22 = texels2.yw;
    store_shared(param_20, param_21, param_22, shared_block);
    uint param_23 = uint(local_coord0_y_half + 8);
    uint param_24 = uint(local_coord0.x + 16);
    half2 param_25 = texels3.xz;
    store_shared(param_23, param_24, param_25, shared_block);
    uint param_26 = uint(local_coord0_y_half + 8);
    uint param_27 = uint(local_coord0.x + 17);
    half2 param_28 = texels3.yw;
    store_shared(param_26, param_27, param_28, shared_block);
    int2 local_coord = int2(int(32u + (2u * (local_index % 4u))), int(2u * (local_index / 4u)));
    int2 param_29 = base_coord + local_coord;
    float2 _364 = generate_mirror_uv(param_29, _106);
    half4 texels = half4(uTexture.gather(uTextureSmplr, _364, int2(0), component::x)).wzxy;
    if (DCShift)
    {
        texels -= half4(half(0.5));
    }
    uint param_30 = uint(local_coord.y >> 1);
    uint param_31 = uint(local_coord.x + 0);
    half2 param_32 = texels.xz;
    store_shared(param_30, param_31, param_32, shared_block);
    uint param_33 = uint(local_coord.y >> 1);
    uint param_34 = uint(local_coord.x + 1);
    half2 param_35 = texels.yw;
    store_shared(param_33, param_34, param_35, shared_block);
    int2 local_coord_1 = int2(int(2u * (local_index % 16u)), int(32u + (2u * (local_index / 16u))));
    int2 param_36 = base_coord + local_coord_1;
    float2 _419 = generate_mirror_uv(param_36, _106);
    half4 texels_1 = half4(uTexture.gather(uTextureSmplr, _419, int2(0), component::x)).wzxy;
    if (DCShift)
    {
        texels_1 -= half4(half(0.5));
    }
    uint param_37 = uint(local_coord_1.y >> 1);
    uint param_38 = uint(local_coord_1.x + 0);
    half2 param_39 = texels_1.xz;
    store_shared(param_37, param_38, param_39, shared_block);
    uint param_40 = uint(local_coord_1.y >> 1);
    uint param_41 = uint(local_coord_1.x + 1);
    half2 param_42 = texels_1.yw;
    store_shared(param_40, param_41, param_42, shared_block);
    if (local_index < 16u)
    {
        int2 local_coord_2 = int2(int(32u + (2u * (local_index % 4u))), int(32u + (2u * (local_index / 4u))));
        int2 param_43 = base_coord + local_coord_2;
        float2 _478 = generate_mirror_uv(param_43, _106);
        half4 texels_2 = half4(uTexture.gather(uTextureSmplr, _478, int2(0), component::x)).wzxy;
        if (DCShift)
        {
            texels_2 -= half4(half(0.5));
        }
        uint param_44 = uint(local_coord_2.y >> 1);
        uint param_45 = uint(local_coord_2.x + 0);
        half2 param_46 = texels_2.xz;
        store_shared(param_44, param_45, param_46, shared_block);
        uint param_47 = uint(local_coord_2.y >> 1);
        uint param_48 = uint(local_coord_2.x + 1);
        half2 param_49 = texels_2.yw;
        store_shared(param_47, param_48, param_49, shared_block);
    }
}

static inline __attribute__((always_inline))
half2 load_shared(thread const uint& y, thread const uint& x, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block)
{
    return shared_block[y][x];
}

static inline __attribute__((always_inline))
void forward_transform8x2(threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(8u * (local_index % 4u)), int(local_index / 4u));
    spvUnsafeArray<half2, 16> values;
    for (int i = 0; i < 16; i++)
    {
        uint param = uint(local_coord.y);
        uint param_1 = uint(local_coord.x + i);
        half2 v = load_shared(param, param_1, shared_block);
        values[i] = v;
    }
    // CDF5/3 (LeGall): donor predict then update, no9/7Krescale.
    for (int i_1 = 1; i_1 < 15; i_1 += 2)
    {
        values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * half(0.5));
    }
    for (int i_2 = 2; i_2 < 14; i_2 += 2)
    {
        values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * half(0.25));
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int i_5 = 2; i_5 < 6; i_5++)
    {
        half2 a = values[(2 * i_5) + 0];
        half2 b = values[(2 * i_5) + 1];
        half2 t0 = half2(a.x, b.x);
        half2 t1 = half2(a.y, b.y);
        int y_coord = (local_coord.x >> 1) + (i_5 - 2);
        uint param_2 = uint(y_coord);
        uint param_3 = uint((2 * local_coord.y) + 0);
        half2 param_4 = t0;
        store_shared(param_2, param_3, param_4, shared_block);
        uint param_5 = uint(y_coord);
        uint param_6 = uint((2 * local_coord.y) + 1);
        half2 param_7 = t1;
        store_shared(param_5, param_6, param_7, shared_block);
    }
}

static inline __attribute__((always_inline))
void forward_transform4x2(thread const bool& active_lane, thread const int& y_offset, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(4u * (local_index % 8u)), int((local_index / 8u) + uint(y_offset)));
    spvUnsafeArray<half2, 12> values;
    if (active_lane)
    {
        for (int i = 0; i < 12; i++)
        {
            uint param = uint(local_coord.y);
            uint param_1 = uint(local_coord.x + i);
            half2 v = load_shared(param, param_1, shared_block);
            values[i] = v;
        }
        // CDF5/3 (LeGall): donor predict then update, no9/7Krescale.
        for (int i_1 = 1; i_1 < 11; i_1 += 2)
        {
            values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * half(0.5));
        }
        for (int i_2 = 2; i_2 < 10; i_2 += 2)
        {
            values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * half(0.25));
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (active_lane)
    {
        for (int i_5 = 2; i_5 < 4; i_5++)
        {
            half2 a = values[(2 * i_5) + 0];
            half2 b = values[(2 * i_5) + 1];
            half2 t0 = half2(a.x, b.x);
            half2 t1 = half2(a.y, b.y);
            int y_coord = (local_coord.x >> 1) + (i_5 - 2);
            uint param_2 = uint(y_coord);
            uint param_3 = uint((2 * local_coord.y) + 0);
            half2 param_4 = t0;
            store_shared(param_2, param_3, param_4, shared_block);
            uint param_5 = uint(y_coord);
            uint param_6 = uint((2 * local_coord.y) + 1);
            half2 param_7 = t1;
            store_shared(param_5, param_6, param_7, shared_block);
        }
    }
}

kernel void pyrowave_dwt(constant Registers& _106 [[buffer(0)]], texture2d<float> uTexture [[texture(0)]], texture2d_array<float, access::write> uOutput [[texture(1)]], sampler uTextureSmplr [[sampler(0)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint gl_SubgroupID [[simdgroup_index_in_threadgroup]], uint gl_SubgroupSize [[threads_per_simdgroup]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])
{
    threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20> shared_block;
    uint local_index = (gl_SubgroupID * gl_SubgroupSize) + gl_SubgroupInvocationID;
    load_image_with_apron(shared_block, _106, gl_WorkGroupID, local_index, uTexture, uTextureSmplr);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    forward_transform8x2(shared_block, local_index);
    bool param = local_index < 32u;
    int param_1 = 16;
    forward_transform4x2(param, param_1, shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    forward_transform8x2(shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint param_2 = local_index;
    int2 local_coord = unswizzle8x8(param_2);
    for (int y = local_coord.y; y < 16; y += 8)
    {
        int _988 = local_coord.x * 2;
        for (int x = _988; x < 32; x += 16)
        {
            uint param_3 = uint(y);
            uint param_4 = uint(x + 0);
            half2 v0 = load_shared(param_3, param_4, shared_block);
            uint param_5 = uint(y);
            uint param_6 = uint(x + 1);
            half2 v1 = load_shared(param_5, param_6, shared_block);
            int img_x = x >> 1;
            int img_y = y;
            int2 base_image_coord = (int2(gl_WorkGroupID.xy) * int2(16)) + int2(img_x, img_y);
            int3 _1037 = int3(base_image_coord, 0);
            uOutput.write(float4(v0.xxxx), uint2(_1037.xy), uint(_1037.z));
            int3 _1045 = int3(base_image_coord, 2);
            uOutput.write(float4(v0.yyyy), uint2(_1045.xy), uint(_1045.z));
            int3 _1053 = int3(base_image_coord, 1);
            uOutput.write(float4(v1.xxxx), uint2(_1053.xy), uint(_1053.z));
            int3 _1061 = int3(base_image_coord, 3);
            uOutput.write(float4(v1.yyyy), uint2(_1061.xy), uint(_1061.z));
        }
    }
}

)CDF53_MSL";
static const char idwt_cdf53_fp16_msl_source[] = R"CDF53_MSL(
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

struct Registers
{
    int2 resolution;
    float2 inv_resolution;
};

constant bool DCShift_tmp [[function_constant(0)]];
constant bool DCShift = is_function_constant_defined(DCShift_tmp) ? DCShift_tmp : false;

static inline __attribute__((always_inline))
int2 unswizzle8x8(thread const uint& index)
{
    uint y = extract_bits(index, uint(0), uint(1));
    uint x = extract_bits(index, uint(1), uint(2));
    y |= (extract_bits(index, uint(3), uint(2)) << uint(1));
    x |= (extract_bits(index, uint(5), uint(1)) << uint(2));
    return int2(int(x), int(y));
}

static inline __attribute__((always_inline))
bool2 band(thread const bool2& a, thread const bool2& b)
{
    return bool2(a.x && b.x, a.y && b.y);
}

static inline __attribute__((always_inline))
float2 generate_mirror_uv(thread int2& coord, thread const bool& even_x, thread const bool& even_y, constant Registers& _150)
{
    bool2 param = bool2(even_x, even_y);
    bool2 param_1 = coord < int2(0);
    coord -= int2(band(param, param_1));
    coord += int2(1);
    bool2 param_2 = bool2(!even_x, !even_y);
    bool2 param_3 = coord >= _150.resolution;
    coord += int2(band(param_2, param_3));
    float2 uv = float2(coord) * _150.inv_resolution;
    return uv.yx;
}

static inline __attribute__((always_inline))
void store_shared(thread const uint& y, thread const uint& x, thread const half2& v, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block)
{
    shared_block[y][x] = v;
}

static inline __attribute__((always_inline))
void write_shared_4x4(thread const int2& coord, thread const half4& texels0, thread const half4& texels1, thread const half4& texels2, thread const half4& texels3, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block)
{
    uint param = uint(coord.y + 0);
    uint param_1 = uint((2 * coord.x) + 0);
    half2 param_2 = half2(texels0.x, texels2.x);
    store_shared(param, param_1, param_2, shared_block);
    uint param_3 = uint(coord.y + 0);
    uint param_4 = uint((2 * coord.x) + 1);
    half2 param_5 = half2(texels1.x, texels3.x);
    store_shared(param_3, param_4, param_5, shared_block);
    uint param_6 = uint(coord.y + 0);
    uint param_7 = uint((2 * coord.x) + 2);
    half2 param_8 = half2(texels0.y, texels2.y);
    store_shared(param_6, param_7, param_8, shared_block);
    uint param_9 = uint(coord.y + 0);
    uint param_10 = uint((2 * coord.x) + 3);
    half2 param_11 = half2(texels1.y, texels3.y);
    store_shared(param_9, param_10, param_11, shared_block);
    uint param_12 = uint(coord.y + 1);
    uint param_13 = uint((2 * coord.x) + 0);
    half2 param_14 = half2(texels0.z, texels2.z);
    store_shared(param_12, param_13, param_14, shared_block);
    uint param_15 = uint(coord.y + 1);
    uint param_16 = uint((2 * coord.x) + 1);
    half2 param_17 = half2(texels1.z, texels3.z);
    store_shared(param_15, param_16, param_17, shared_block);
    uint param_18 = uint(coord.y + 1);
    uint param_19 = uint((2 * coord.x) + 2);
    half2 param_20 = half2(texels0.w, texels2.w);
    store_shared(param_18, param_19, param_20, shared_block);
    uint param_21 = uint(coord.y + 1);
    uint param_22 = uint((2 * coord.x) + 3);
    half2 param_23 = half2(texels1.w, texels3.w);
    store_shared(param_21, param_22, param_23, shared_block);
}

static inline __attribute__((always_inline))
void load_image_with_apron(threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, constant Registers& _150, thread uint3& gl_WorkGroupID, thread uint& local_index, texture2d_array<float> uTexture, sampler uTextureSmplr)
{
    int2 base_coord = (int2(gl_WorkGroupID.xy) * int2(16)) - int2(2);
    uint param = local_index;
    int2 local_coord0 = int2(2) * unswizzle8x8(param);
    int2 coord0 = base_coord + local_coord0;
    int2 param_1 = coord0;
    bool param_2 = true;
    bool param_3 = true;
    float2 _356 = generate_mirror_uv(param_1, param_2, param_3, _150);
    float3 _361 = float3(_356, 0.0);
    half4 texels0 = half4(uTexture.gather(uTextureSmplr, _361.xy, uint(rint(_361.z)), int2(0), component::x)).wxzy;
    int2 param_4 = coord0;
    bool param_5 = false;
    bool param_6 = true;
    float2 _373 = generate_mirror_uv(param_4, param_5, param_6, _150);
    float3 _377 = float3(_373, 2.0);
    half4 texels1 = half4(uTexture.gather(uTextureSmplr, _377.xy, uint(rint(_377.z)), int2(0), component::x)).wxzy;
    int2 param_7 = coord0;
    bool param_8 = true;
    bool param_9 = false;
    float2 _387 = generate_mirror_uv(param_7, param_8, param_9, _150);
    float3 _391 = float3(_387, 1.0);
    half4 texels2 = half4(uTexture.gather(uTextureSmplr, _391.xy, uint(rint(_391.z)), int2(0), component::x)).wxzy;
    int2 param_10 = coord0;
    bool param_11 = false;
    bool param_12 = false;
    float2 _401 = generate_mirror_uv(param_10, param_11, param_12, _150);
    float3 _405 = float3(_401, 3.0);
    half4 texels3 = half4(uTexture.gather(uTextureSmplr, _405.xy, uint(rint(_405.z)), int2(0), component::x)).wxzy;
    int2 param_13 = local_coord0;
    half4 param_14 = texels0;
    half4 param_15 = texels1;
    half4 param_16 = texels2;
    half4 param_17 = texels3;
    write_shared_4x4(param_13, param_14, param_15, param_16, param_17, shared_block);
    int2 local_coord_horiz = int2(int(16u + (2u * (local_index % 2u))), int(2u * (local_index / 2u)));
    if (local_coord_horiz.y < 20)
    {
        int2 param_18 = base_coord + local_coord_horiz;
        bool param_19 = true;
        bool param_20 = true;
        float2 _445 = generate_mirror_uv(param_18, param_19, param_20, _150);
        float3 _448 = float3(_445, 0.0);
        texels0 = half4(uTexture.gather(uTextureSmplr, _448.xy, uint(rint(_448.z)), int2(0), component::x)).wxzy;
        int2 param_21 = base_coord + local_coord_horiz;
        bool param_22 = false;
        bool param_23 = true;
        float2 _459 = generate_mirror_uv(param_21, param_22, param_23, _150);
        float3 _462 = float3(_459, 2.0);
        texels1 = half4(uTexture.gather(uTextureSmplr, _462.xy, uint(rint(_462.z)), int2(0), component::x)).wxzy;
        int2 param_24 = base_coord + local_coord_horiz;
        bool param_25 = true;
        bool param_26 = false;
        float2 _473 = generate_mirror_uv(param_24, param_25, param_26, _150);
        float3 _476 = float3(_473, 1.0);
        texels2 = half4(uTexture.gather(uTextureSmplr, _476.xy, uint(rint(_476.z)), int2(0), component::x)).wxzy;
        int2 param_27 = base_coord + local_coord_horiz;
        bool param_28 = false;
        bool param_29 = false;
        float2 _487 = generate_mirror_uv(param_27, param_28, param_29, _150);
        float3 _490 = float3(_487, 3.0);
        texels3 = half4(uTexture.gather(uTextureSmplr, _490.xy, uint(rint(_490.z)), int2(0), component::x)).wxzy;
        int2 param_30 = local_coord_horiz;
        half4 param_31 = texels0;
        half4 param_32 = texels1;
        half4 param_33 = texels2;
        half4 param_34 = texels3;
        write_shared_4x4(param_30, param_31, param_32, param_33, param_34, shared_block);
    }
    int2 local_coord_vert = local_coord_horiz.yx;
    if (local_coord_vert.x < 16)
    {
        int2 param_35 = base_coord + local_coord_vert;
        bool param_36 = true;
        bool param_37 = true;
        float2 _520 = generate_mirror_uv(param_35, param_36, param_37, _150);
        float3 _523 = float3(_520, 0.0);
        texels0 = half4(uTexture.gather(uTextureSmplr, _523.xy, uint(rint(_523.z)), int2(0), component::x)).wxzy;
        int2 param_38 = base_coord + local_coord_vert;
        bool param_39 = false;
        bool param_40 = true;
        float2 _534 = generate_mirror_uv(param_38, param_39, param_40, _150);
        float3 _537 = float3(_534, 2.0);
        texels1 = half4(uTexture.gather(uTextureSmplr, _537.xy, uint(rint(_537.z)), int2(0), component::x)).wxzy;
        int2 param_41 = base_coord + local_coord_vert;
        bool param_42 = true;
        bool param_43 = false;
        float2 _548 = generate_mirror_uv(param_41, param_42, param_43, _150);
        float3 _551 = float3(_548, 1.0);
        texels2 = half4(uTexture.gather(uTextureSmplr, _551.xy, uint(rint(_551.z)), int2(0), component::x)).wxzy;
        int2 param_44 = base_coord + local_coord_vert;
        bool param_45 = false;
        bool param_46 = false;
        float2 _562 = generate_mirror_uv(param_44, param_45, param_46, _150);
        float3 _565 = float3(_562, 3.0);
        texels3 = half4(uTexture.gather(uTextureSmplr, _565.xy, uint(rint(_565.z)), int2(0), component::x)).wxzy;
        int2 param_47 = local_coord_vert;
        half4 param_48 = texels0;
        half4 param_49 = texels1;
        half4 param_50 = texels2;
        half4 param_51 = texels3;
        write_shared_4x4(param_47, param_48, param_49, param_50, param_51, shared_block);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
}

static inline __attribute__((always_inline))
half2 load_shared(thread const uint& y, thread const uint& x, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block)
{
    return shared_block[y][x];
}

static inline __attribute__((always_inline))
void inverse_transform8x2(threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(8u * (local_index % 4u)), int(local_index / 4u));
    spvUnsafeArray<half2, 16> values;
    for (int i = 0; i < 16; i += 2)
    {
        uint param = uint(local_coord.y);
        uint param_1 = uint((local_coord.x + i) + 0);
        half2 v0 = load_shared(param, param_1, shared_block);
        uint param_2 = uint(local_coord.y);
        uint param_3 = uint((local_coord.x + i) + 1);
        half2 v1 = load_shared(param_2, param_3, shared_block);
        values[i + 0] = v0;
        values[i + 1] = v1;
    }
    // CDF5/3: undo update then predict, matching donor inverse.
    for (int i_1 = 2; i_1 < 15; i_1 += 2)
    {
        values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * half(0.25));
    }
    for (int i_2 = 3; i_2 < 14; i_2 += 2)
    {
        values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * half(0.5));
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int i_5 = 2; i_5 < 6; i_5++)
    {
        half2 a = values[(2 * i_5) + 0];
        half2 b = values[(2 * i_5) + 1];
        half2 t0 = half2(a.x, b.x);
        half2 t1 = half2(a.y, b.y);
        int y_coord = (local_coord.x >> 1) + (i_5 - 2);
        uint param_4 = uint(y_coord);
        uint param_5 = uint((2 * local_coord.y) + 0);
        half2 param_6 = t0;
        store_shared(param_4, param_5, param_6, shared_block);
        uint param_7 = uint(y_coord);
        uint param_8 = uint((2 * local_coord.y) + 1);
        half2 param_9 = t1;
        store_shared(param_7, param_8, param_9, shared_block);
    }
}

static inline __attribute__((always_inline))
void inverse_transform4x2(thread const bool& active_lane, thread const int& y_offset, threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20>& shared_block, thread uint& local_index)
{
    int2 local_coord = int2(int(4u * (local_index % 8u)), int((local_index / 8u) + uint(y_offset)));
    spvUnsafeArray<half2, 12> values;
    if (active_lane)
    {
        for (int i = 0; i < 12; i += 2)
        {
            uint param = uint(local_coord.y);
            uint param_1 = uint((local_coord.x + i) + 0);
            half2 v0 = load_shared(param, param_1, shared_block);
            uint param_2 = uint(local_coord.y);
            uint param_3 = uint((local_coord.x + i) + 1);
            half2 v1 = load_shared(param_2, param_3, shared_block);
            values[i + 0] = v0;
            values[i + 1] = v1;
        }
        // CDF5/3: undo update then predict, matching donor inverse.
        for (int i_1 = 2; i_1 < 11; i_1 += 2)
        {
            values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * half(0.25));
        }
        for (int i_2 = 3; i_2 < 10; i_2 += 2)
        {
            values[i_2] += ((values[i_2 - 1] + values[i_2 + 1]) * half(0.5));
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (active_lane)
    {
        for (int i_5 = 2; i_5 < 4; i_5++)
        {
            half2 a = values[(2 * i_5) + 0];
            half2 b = values[(2 * i_5) + 1];
            half2 t0 = half2(a.x, b.x);
            half2 t1 = half2(a.y, b.y);
            int y_coord = (local_coord.x >> 1) + (i_5 - 2);
            uint param_4 = uint(y_coord);
            uint param_5 = uint((2 * local_coord.y) + 0);
            half2 param_6 = t0;
            store_shared(param_4, param_5, param_6, shared_block);
            uint param_7 = uint(y_coord);
            uint param_8 = uint((2 * local_coord.y) + 1);
            half2 param_9 = t1;
            store_shared(param_7, param_8, param_9, shared_block);
        }
    }
}

kernel void pyrowave_idwt(constant Registers& _150 [[buffer(0)]], texture2d_array<float> uTexture [[texture(0)]], texture2d<float, access::write> uOutput [[texture(1)]], sampler uTextureSmplr [[sampler(0)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]])
{
    threadgroup spvUnsafeArray<spvUnsafeArray<half2, 41>, 20> shared_block;
    uint local_index = gl_LocalInvocationIndex;
    load_image_with_apron(shared_block, _150, gl_WorkGroupID, local_index, uTexture, uTextureSmplr);
    inverse_transform8x2(shared_block, local_index);
    bool param = local_index < 32u;
    int param_1 = 16;
    inverse_transform4x2(param, param_1, shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    inverse_transform8x2(shared_block, local_index);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint param_2 = local_index;
    int2 local_coord = unswizzle8x8(param_2);
    for (int y = local_coord.y; y < 16; y += 8)
    {
        for (int x = local_coord.x; x < 32; x += 8)
        {
            uint param_3 = uint(y);
            uint param_4 = uint(x);
            half2 v = load_shared(param_3, param_4, shared_block);
            if (DCShift)
            {
                v += half2(half(0.5));
            }
            uOutput.write(float4(v.xxxx), uint2((int2((2 * y) + 0, x) + (int2(32) * int2(gl_WorkGroupID.yx)))));
            uOutput.write(float4(v.yyyy), uint2((int2((2 * y) + 1, x) + (int2(32) * int2(gl_WorkGroupID.yx)))));
        }
    }
}

)CDF53_MSL";
}
