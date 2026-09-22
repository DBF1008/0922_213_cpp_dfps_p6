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

#include "dynamic_fps.h"
#include "cobridge_type.h"
#include "utils/fmt_exception.h"
#include "utils/misc.h"
#include "utils/misc_android.h"
#include "utils/switch_verify.h"
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <spdlog/spdlog.h>

constexpr char MODULE_NAME[] = "DynamicFps";
constexpr int64_t DEFAULT_GESTURE_SLACK_MS = 4000;
constexpr int64_t DEFAULT_TOUCH_SLACK_MS = 4000;
constexpr int DEFAULT_ENABLE_MIN_BRIGHTNESS = 8;
constexpr bool DEFAULT_USE_SF_BACKDOOR = false;
constexpr int MIN_TOUCH_SLACK_MS = 100;
constexpr int MAX_ENABLE_MIN_BRIGHTNESS = 255;
constexpr double BRIGHTNESS_SAMPLE_INTERVAL_S = 10;
constexpr char UNIVERSIAL_PKG_NAME[] = "*";
constexpr char OFFSCREEN_PKG_NAME[] = "-";

std::string Trim(const std::string &str) {
    if (str.empty()) {
        return str;
    }
    auto firstScan = str.find_first_not_of(" \n\r");
    auto first = (firstScan == std::string::npos) ? str.length() : firstScan;
    auto last = str.find_last_not_of(" \n\r");
    return str.substr(first, last - first + 1);
}

// Strict integer parser that rejects the entire string on any error
// (empty, non-numeric, trailing garbage, overflow).
// Returns {value, true} on success, {0, false} on failure.
static std::pair<int, bool> SafeStoi(const std::string &s) {
    if (s.empty()) {
        return {0, false};
    }
    const char *begin = s.c_str();
    char *end = nullptr;
    errno = 0;
    long val = std::strtol(begin, &end, 10);
    if (end == begin || *end != '\0' || errno == ERANGE || val < INT_MIN || val > INT_MAX) {
        return {0, false};
    }
    return {static_cast<int>(val), true};
}

DynamicFps::DynamicFps(const std::string &configPath, const std::string &notifyPath)
    : useSfBackdoor_(DEFAULT_USE_SF_BACKDOOR),
      touchSlackMs_(DEFAULT_TOUCH_SLACK_MS),
      gestureSlackMs_(DEFAULT_GESTURE_SLACK_MS),
      enableMinBrightness_(DEFAULT_ENABLE_MIN_BRIGHTNESS),
      hasUniversial_(false),
      hasOffscreen_(false),
      notifyPath_(notifyPath),
      touchPressed_(false),
      btnPressed_(false),
      active_(false),
      lowBrightness_(false),
      isOffscreen_(false),
      curHz_(INT32_MAX),
      forceSwitch_(false),
      dwInput_(DwCreate(MODULE_NAME)),
      dwGesture_(DwCreate(MODULE_NAME)),
      dwWakeup_(DwCreate(MODULE_NAME)),
      hw_(HwCreate(MODULE_NAME)) {
    LoadConfig(configPath);
}

void DynamicFps::Start(void) { AddReactor(); }

void DynamicFps::LoadConfig(const std::string &configPath) {
    FILE *fp = fopen(configPath.c_str(), "re");
    if (fp == NULL) {
        throw FmtException("Cannot open config '{}'", configPath);
    }

    std::vector<std::string> errors;
    char buf[256];
    int lineNum = 0;
    while (feof(fp) == false) {
        buf[0] = '\0';
        fgets(buf, sizeof(buf), fp);
        ++lineNum;
        auto line = Trim(buf);
        ParseLine(line, lineNum, errors);
    }

    if (hasOffscreen_ == false) {
        errors.push_back("offscreen rule '-' not specified");
    }
    if (hasUniversial_ == false) {
        errors.push_back("default rule '*' not specified");
    }

    auto ruleErrors = FindInvalidRules();
    errors.insert(errors.end(), ruleErrors.begin(), ruleErrors.end());

    if (errors.empty() == false) {
        fclose(fp);
        for (const auto &e : errors) {
            SPDLOG_ERROR("Config error: {}", e);
        }
        throw FmtException("Config '{}' has {} error(s), keeping defaults", configPath, errors.size());
    }

    if (useSfBackdoor_) {
        SPDLOG_INFO("Use surfaceflinger backdoor to switch refresh rate");
    } else {
        SPDLOG_INFO("Use PEAK_REFRESH_RATE to switch refresh rate");
    }

    fclose(fp);
}

void DynamicFps::ParseLine(const std::string &line, int lineNum, std::vector<std::string> &errors) {
    auto isComment = [](const std::string &line) { return line[0] == '#'; };
    auto isTunable = [](const std::string &line) { return line[0] == '/'; };

    char name[256];
    char value[256];
    name[0] = '\0';
    value[0] = '\0';

    if (line.empty() || isComment(line)) {
        return;
    } else if (isTunable(line)) {
        // /touchSlackMs 4000
        if (sscanf(line.c_str(), "/%s %s", name, value) == 2) {
            SPDLOG_DEBUG("Set '{}'={}", name, value);
            SetTunable(name, value, lineNum, errors);
        } else {
            errors.push_back(fmt::format("line {}: malformed tunable '{}'", lineNum, line));
        }
    } else {
        // com.example.app 60 120
        // com.example.app 2 0
        FpsRule rule;
        if (sscanf(line.c_str(), "%s %d %d", name, &rule.idle, &rule.active) == 3) {
            AddRule(name, rule);
        } else {
            errors.push_back(fmt::format("line {}: malformed rule '{}'", lineNum, line));
        }
    }
}

void DynamicFps::AddRule(const std::string &pkgName, FpsRule rule) {
    auto isUniversial = [](const std::string &pkgName) { return pkgName == UNIVERSIAL_PKG_NAME; };
    auto isOffscreen = [](const std::string &pkgName) { return pkgName == OFFSCREEN_PKG_NAME; };
    if (isUniversial(pkgName)) {
        hasUniversial_ = true;
        universial_ = rule;
    } else if (isOffscreen(pkgName)) {
        hasOffscreen_ = true;
        offscreen_ = rule;
    } else {
        SPDLOG_DEBUG("Load '{}', dfps={}/{}", pkgName, rule.idle, rule.active);
        rules_.emplace(pkgName, rule);
    }
}

void DynamicFps::SetTunable(const std::string &tunable, const std::string &value, int lineNum,
                            std::vector<std::string> &errors) {
    auto [parsed, ok] = SafeStoi(value);
    if (!ok) {
        errors.push_back(fmt::format("line {}: tunable '{}': '{}' is not a valid integer", lineNum, tunable, value));
        return;
    }

    if (tunable == "useSfBackdoor") {
        useSfBackdoor_ = (parsed > 0) ? true : false;
    } else if (tunable == "touchSlackMs") {
        touchSlackMs_ = std::max(MIN_TOUCH_SLACK_MS, parsed);
    } else if (tunable == "enableMinBrightness") {
        enableMinBrightness_ = std::min(MAX_ENABLE_MIN_BRIGHTNESS, parsed);
    } else {
        errors.push_back(fmt::format("line {}: unknown tunable '{}'", lineNum, tunable));
    }
}

std::vector<std::string> DynamicFps::FindInvalidRules(void) {
    std::vector<std::string> errors;
    auto isDefaultRule = [](const FpsRule &rule) { return rule.idle == -1 && rule.active == -1; };
    auto isSfBackdoorRule = [](const FpsRule &rule) { return rule.idle < 20 && rule.active < 20; };
    auto isInvalid = [=](const FpsRule &rule) {
        return isDefaultRule(rule) == false && useSfBackdoor_ != isSfBackdoorRule(rule);
    };

    if (isInvalid(offscreen_)) {
        errors.push_back("offscreen rule '-' has invalid refresh rate values");
    }
    if (isInvalid(universial_)) {
        errors.push_back("default rule '*' has invalid refresh rate values");
    }
    for (const auto &[name, rule] : rules_) {
        if (isInvalid(rule)) {
            errors.push_back(fmt::format("rule '{}': invalid refresh rate values", name));
        }
    }

    return errors;
}

DynamicFps::FpsRule DynamicFps::GetCurrentRule(void) const {
    FpsRule rule;
    const auto &pkgName = overridedApp_.empty() ? curApp_ : overridedApp_;
    if (pkgName == OFFSCREEN_PKG_NAME) {
        rule = offscreen_;
    } else {
        auto it = rules_.find(pkgName);
        if (it != rules_.end()) {
            rule = it->second;
        } else {
            rule = universial_;
        }
    }
    return rule;
}

void DynamicFps::AddReactor(void) {
    using namespace std::placeholders;
    CoSubscribe("input.touch", std::bind(&DynamicFps::OnInputTouch, this, _1));
    CoSubscribe("input.btn", std::bind(&DynamicFps::OnInputBtn, this, _1));
    CoSubscribe("input.state", std::bind(&DynamicFps::OnInputScene, this, _1));
    CoSubscribe("topapp.pkgName", std::bind(&DynamicFps::OnTopAppSwitch, this, _1));
    CoSubscribe("offscreen.state", std::bind(&DynamicFps::OnOffscreen, this, _1));
}

void DynamicFps::OnInputTouch(const void *data) {
    const auto &pressed = CoBridge::Get<bool>(data);
    touchPressed_ = pressed;
    OnInput();
}

void DynamicFps::OnInputBtn(const void *data) {
    const auto &pressed = CoBridge::Get<bool>(data);
    btnPressed_ = pressed;
    OnInput();
}

void DynamicFps::OnInput(void) {
    auto pressed = touchPressed_ || btnPressed_;
    if (pressed) {
        active_ = true;
        SwitchRefreshRate();
        DwSetWork(dwInput_, nullptr, DelayedWorker::SLEEP_TS);
    } else {
        auto enterIdle = [=]() {
            active_ = false;
            SwitchRefreshRate();
        };
        DwSetWork(dwInput_, enterIdle, GetNowTs() + MsToUs(touchSlackMs_));
    }
}

void DynamicFps::OnInputScene(const void *data) {
    const auto &input = CoBridge::Get<InputData>(data);
    if (input.inGesture) {
        overridedApp_ = UNIVERSIAL_PKG_NAME;
        SwitchRefreshRate();
        DwSetWork(dwGesture_, nullptr, DelayedWorker::SLEEP_TS);
    } else {
        auto resume = [=]() {
            if (overridedApp_ == UNIVERSIAL_PKG_NAME) {
                overridedApp_ = "";
                SwitchRefreshRate();
            }
        };
        DwSetWork(dwGesture_, resume, GetNowTs() + MsToUs(gestureSlackMs_));
    }
}

void DynamicFps::OnTopAppSwitch(const void *data) {
    const auto &topApp = CoBridge::Get<std::string>(data);
    if (topApp != curApp_) {
        curApp_ = topApp;
        SwitchRefreshRate(true);
    }
}

void DynamicFps::OnOffscreen(const void *data) {
    const auto &isOff = CoBridge::Get<bool>(data);
    if (isOff == isOffscreen_) {
        return;
    }

    isOffscreen_ = isOff;
    if (isOff) {
        overridedApp_ = OFFSCREEN_PKG_NAME;
        SwitchRefreshRate(true);
    } else {
        auto exitOffscreen = [=]() {
            if (overridedApp_ == OFFSCREEN_PKG_NAME) {
                overridedApp_ = "";
                SwitchRefreshRate(true);
            }
        };
        DwSetWork(dwWakeup_, exitOffscreen, GetNowTs() + MsToUs(gestureSlackMs_));
    }
}

void DynamicFps::SwitchRefreshRate(bool force) {
    forceSwitch_ = force;
    if (active_) {
        HwSetWork(hw_, [this]() {
            auto rule = GetCurrentRule();
            SwitchRefreshRate(rule.active);
        });
    } else {
        HwSetWork(hw_, [this]() {
            auto rule = GetCurrentRule();
            if (brightnessTimer_.ElapsedS() > BRIGHTNESS_SAMPLE_INTERVAL_S) {
                brightnessTimer_.Reset();
                auto brightness = GetScreenBrightness();
                lowBrightness_ = brightness < enableMinBrightness_;
            }
            SwitchRefreshRate(lowBrightness_ ? rule.active : rule.idle);
        });
    }
}

void DynamicFps::SwitchRefreshRate(int hz) {
    auto force = forceSwitch_;
    forceSwitch_ = false;
    SPDLOG_DEBUG("switch {}", hz);
    if (force == false && hz == curHz_) {
        return;
    }

    // attempt -> verify -> degrade to the other backend -> rollback
    auto applyBackend = [force](int v, bool useSfBackdoor) {
        auto vStr = std::to_string(v);
        return useSfBackdoor ? SysSurfaceflingerBackdoor(vStr, force) : SysPeakRefreshRate(vStr, force);
    };

    auto result = TrySwitchWithFallback(hz, useSfBackdoor_, applyBackend);
    switch (result) {
        case SwitchBackendResult::kPrimary:
            break;
        case SwitchBackendResult::kFallback:
            useSfBackdoor_ = !useSfBackdoor_;
            SPDLOG_WARN("Primary refresh-rate backend failed, degraded to {}",
                        useSfBackdoor_ ? "surfaceflinger backdoor" : "PEAK_REFRESH_RATE");
            break;
        case SwitchBackendResult::kFailed:
            SPDLOG_ERROR("Cannot switch refresh rate to {}, rolling back", hz);
            RollbackRefreshRate();
            return;
    }

    // only publish the new rate after it was verified on-device
    curHz_ = hz;
    NotifyRefreshRate(std::to_string(hz));
}

void DynamicFps::RollbackRefreshRate(void) {
    if (curHz_ == INT32_MAX) {
        return; // no known-good rate to roll back to
    }

    auto hzStr = std::to_string(curHz_);
    bool ok = useSfBackdoor_ ? SysSurfaceflingerBackdoor(hzStr, true) : SysPeakRefreshRate(hzStr, true);
    if (ok) {
        SPDLOG_INFO("Rolled back to previous refresh rate {}", curHz_);
    } else {
        // actual rate is unknown now; force re-apply on the next switch
        SPDLOG_ERROR("Rollback to {} failed, refresh rate state unknown", curHz_);
        curHz_ = INT32_MAX;
    }
}

void DynamicFps::NotifyRefreshRate(const std::string_view &hz) {
    int fd = open(notifyPath_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_CREAT | O_TRUNC);
    if (fd > 0) {
        WriteSysfsFile(fd, hz);
        close(fd);
    }
}
