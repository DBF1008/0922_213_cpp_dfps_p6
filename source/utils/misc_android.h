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

#include <string>

int GetOSVersion(void);
std::string GetTopAppNameDumpsys(void);
std::string GetHomePackageName(void);
std::string GetTombstone(int pid);
int GetScreenBrightness(void);

// Value validators shared by the switch backends and the fallback logic.
// PeakRefreshRate accepts Hz values: -1 (system default) or >= 20.
// SurfaceflingerBackdoor accepts display config indices: -1 (reset) or [0, 16].
bool IsValidPeakRefreshRateValue(const std::string &hz);
bool IsValidSfBackdoorIdxValue(const std::string &idx);

// Try to switch the refresh rate and verify the result.
// Returns true only when the value was accepted AND verified, so the caller
// can fall back to the other backend or roll back instead of faking success.
bool SysPeakRefreshRate(const std::string &hz, bool force);
bool SysSurfaceflingerBackdoor(const std::string &idx, bool force);
