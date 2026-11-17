// SPDX-License-Identifier: MIT
// Source integration selects the measured batch path explicitly. No app IDs,
// binary hooks or framed preflight are selected automatically.
#pragma once
#include <cstdlib>
#include <cstring>
namespace Mvr9Policy {
inline bool enabled(const char* key) {
    const char* value = getenv(key);
    return value && !strcmp(value,"1");
}
}
