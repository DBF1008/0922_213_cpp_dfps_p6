/*
 * Copyright (C) 2021-2022 Matt Yang
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdlib>
#include <string>

// Pure, platform-independent helpers used to verify whether a refresh-rate
// switch actually took effect, and to decide the fallback/degrade chain.
// Kept free of Android dependencies so they can be unit-tested on host.

// Which backend ended up applying the requested refresh rate.
enum class SwitchBackendResult {
    kPrimary,  // configured backend applied and verified
    kFallback, // configured backend failed, alternate backend applied and verified
    kFailed,   // neither backend could apply the request
};

// Tolerantly parses an integer refresh rate / config index from command
// output, e.g. "60", "-1", " 120\n", "60.0". Rejects empty/non-numeric
// output such as "null".
inline bool ParseHzValue(const std::string &s, int *hz) {
    if (hz == nullptr) {
        return false;
    }
    auto pos = s.find_first_of("-0123456789");
    if (pos == std::string::npos) {
        return false;
    }
    const char *begin = s.c_str() + pos;
    char *end = nullptr;
    long val = std::strtol(begin, &end, 10);
    if (end == begin) {
        return false;
    }
    *hz = static_cast<int>(val);
    return true;
}

// Checks whether the value read back from system settings matches the one
// we tried to write.
inline bool VerifySettingsReadback(const std::string &want, const std::string &got) {
    int wantHz = 0;
    int gotHz = 0;
    if (ParseHzValue(want, &wantHz) == false || ParseHzValue(got, &gotHz) == false) {
        return false;
    }
    return wantHz == gotHz;
}

// Judges whether `service call SurfaceFlinger ...` succeeded from its exit
// status and textual output. A successful call prints "Result: Parcel(...)";
// failures print nothing useful or an error like "does not exist" /
// "Exception" / "error".
inline bool IsServiceCallResultOk(int exitStatus, const std::string &output) {
    if (exitStatus != 0) {
        return false;
    }
    if (output.find("Parcel") == std::string::npos) {
        return false;
    }
    auto hasError = output.find("error") != std::string::npos ||
                    output.find("Error") != std::string::npos ||
                    output.find("Exception") != std::string::npos ||
                    output.find("not found") != std::string::npos ||
                    output.find("does not exist") != std::string::npos;
    return hasError == false;
}

// Checks whether a refresh-rate value is representable by a backend.
// The settings backend takes real Hz (>= 20), the surfaceflinger backdoor
// takes a display config index (< 20); -1 (system default) fits both.
inline bool IsRateValidForBackend(int hz, bool useSfBackdoor) {
    if (hz == -1) {
        return true;
    }
    return useSfBackdoor ? (hz >= 0 && hz < 20) : (hz >= 20);
}

// End-to-end attempt chain: try the preferred backend, and if it fails
// verification, degrade to the alternate backend when the value is
// representable there. applyBackend(hz, useSfBackdoor) must return true
// only when the switch was applied AND verified.
template <typename ApplyBackend>
SwitchBackendResult TrySwitchWithFallback(int hz, bool preferSfBackdoor, ApplyBackend &&applyBackend) {
    if (applyBackend(hz, preferSfBackdoor)) {
        return SwitchBackendResult::kPrimary;
    }
    if (IsRateValidForBackend(hz, !preferSfBackdoor) && applyBackend(hz, !preferSfBackdoor)) {
        return SwitchBackendResult::kFallback;
    }
    return SwitchBackendResult::kFailed;
}
