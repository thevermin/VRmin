// SPDX-License-Identifier: MIT
#pragma once

#include <cstdlib>
#include <cstring>

namespace PyroWave {
inline bool cdf53_enabled()
{
    static const bool fastProfile = [] {
        const auto* profile = std::getenv("PYROWAVE_WAVELET");
        return profile && std::strcmp(profile, "53") == 0;
    }();
    return fastProfile;
}
}
