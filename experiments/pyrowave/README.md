# PyroWave source kit for VRmin

Optional wired game-stream codec experiment. This kit contains an Apple Silicon Metal codec and a matching Quest Vulkan decoder patch. Integrating it requires changes to both VRmin endpoints. The desktop stream and current H.264/HEVC paths can keep their existing encoders.

- **Metal** builds a standalone library with GPU NV12/IOSurface input, frame byte-budget control and packetization. It includes CDF 5/3 and CDF 9/7. The CDF 5/3 path includes corrected weighted-discard distortion accounting and luma rate/distortion estimation.
- **Vulkan** adds CDF 5/3 decode with fused final inverse transform and RGBA output, fewer scratch images on the separate CDF 9/7 path, and opt-in packet batching with fallback to the regular parser.

## Build and patch

From this directory on an Apple Silicon Mac:

```sh
cmake -S . -B build/checks
cmake --build build/checks --parallel
ctest --test-dir build/checks --output-on-failure

cmake -S metal -B build/metal -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/metal --parallel

git clone https://github.com/Themaister/pyrowave.git build/pyrowave
git -C build/pyrowave checkout --detach d2997ac172bdc00e29c58e3f2938acb7e94580bf
git -C build/pyrowave apply --check "$PWD/patches/vulkan-backend.patch" \
  "$PWD/patches/vulkan-generated-shaders.patch"
git -C build/pyrowave apply "$PWD/patches/vulkan-backend.patch" \
  "$PWD/patches/vulkan-generated-shaders.patch"
```

With Android NDK r27d available at `ANDROID_NDK`:

```sh
git clone https://github.com/Themaister/Granite.git build/pyrowave/Granite
git -C build/pyrowave/Granite checkout --detach 842d9d5686ba8c799a7d34a78a68f98d6aeb5a68
git -C build/pyrowave/Granite submodule update --init \
  third_party/volk third_party/khronos/vulkan-headers
cmake -S build/pyrowave -B build/quest \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DPYROWAVE_DEVEL=OFF -DPYROWAVE_FP32_MATH=ON
cmake --build build/quest --target pyrowave-shared --parallel
```

[PROVENANCE.json](PROVENANCE.json) records the source pins. Apply both patches. The backend patch contains the implementation and shader sources. The generated patch contains the shader headers required to build without running the shader toolchain.

## VRmin integration

1. **Select a matched profile.** Extend the Quest auth/config exchange in `mac/renderer/main.swift` and `quest-app/app/src/main/cpp/main.cpp` with an explicit codec and profile capability. Select PyroWave for a wired connection only when both sides accept the same profile. Keep legacy clients on the current H.264/HEVC flow. Start with the CDF 5/3, 4:2:0, RGBA-output profile. Set `PYROWAVE_WAVELET=53` before the Metal process initializes the codec and create the Quest decoder with `wavelet = PYROWAVE_WAVELET_CDF53` and `fragment_path = false`. The sender library defaults to CDF 9/7.
2. **Connect the Metal encoder.** Convert the existing stereo game BGRA texture into a full-range BT.709 NV12 IOSurface using a Metal pass. Align the 4:2:0 sampling grid to each eye. Feed that surface into `pyrowave_encoder_encode_gpu` after the conversion has completed. Send packetized frame data with the same frame/pose IDs and an explicit stream-profile ID. Derive the per-frame byte budget from the requested bitrate and refresh rate. Retain a bounded queue since extracting packets waits for GPU completion.
3. **Connect the Vulkan decoder.** Route negotiated PyroWave packets around `AMediaCodec` with explicit frame boundaries and length checks. For the included fused CDF 5/3 path, use dimensions divisible by four and request `pyrowave_decoder_set_cdf53v2(decoder, 8)` and verify success. Use the full-size RGBA output with a GPU-backed buffer shared with GLES or an equivalent renderer-owned import. Preserve readiness and frame IDs and keep each image until Quest presentation has finished sampling it. The Vulkan decoder produces the image; VRmin still needs the presentation adapter and fence handling.
4. **Run a wired A/B.** Compare against the existing game codec using the same scene, dimensions, refresh rate and foveation. Record encode and decode timing, actual wire bitrate, presented FPS, drops and image quality. Also reconnect once and verify H.264/HEVC fallback with an older client. Keep packet batching off for the first run; it can be enabled independently with `PYROWAVE_V9_PACKET_BATCH=1` after a working baseline.

## Validation

- ARM64 Metal library built on macOS.
- Five standalone CPU checks passed.
- Vulkan patch applied cleanly to the pinned upstream commit. `libpyrowave-shared.so` built for Android ARM64 (API 30, NDK r27d).
- VRmin transport and headset presentation still need integration. An end-to-end Quest run is pending.

## Upstream

- [PyroWave by Hans-Kristian Arntzen](https://github.com/Themaister/pyrowave)
- [Quest3-Pyrowave CDF 5/3 work by JMS1717](https://github.com/JMS1717/Quest3-Pyrowave)
- [Granite](https://github.com/Themaister/Granite)

MIT notices and dependency licenses accompany the sources.
