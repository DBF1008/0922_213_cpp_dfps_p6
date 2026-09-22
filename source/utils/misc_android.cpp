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

#include "misc_android.h"
#include "utils/misc.h"
#include <cerrno>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <spdlog/spdlog.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static int androidOsVersion;

int GetOSVersion(void) {
    if (androidOsVersion) {
        return androidOsVersion;
    }

    char version[PROP_VALUE_MAX + 1];
    __system_property_get("ro.build.version.release", version);
    androidOsVersion = atoi(version);

    return androidOsVersion;
}

std::string GetTopAppNameDumpsysAndroid7(void) {
    // venus:/ # dumpsys activity o | grep top-activity
    //     Proc # 3: fg     T/A/TOP  LCM  t: 0 30265:com.tencent.mm:toolsmp/u0a272 (top-activity)
    // venus:/ # dumpsys activity o | grep top-activity
    //     Proc # 0: fg     T/A/TOP  LCM  t: 0 2849:com.miui.home/u0a91 (top-activity)
    // osborn:/ # dumpsys activity o | grep top-activity
    //     Proc # 1: fore  T/A/TOP  trm: 0 2899:com.teslacoilsw.launcher/u0a178 (top-activity)
    std::string buf;
    ExecCmdSync(&buf, "/system/bin/dumpsys", "activity", "o");
    if (buf.empty()) {
        return {};
    }

    constexpr char TA_KEYWORD[] = " (top-activity)";
    char *topappName = strstr(buf.data(), TA_KEYWORD);
    if (topappName == nullptr) {
        return {};
    }
    topappName--;
    while (topappName > buf.data() && *topappName != ' ') {
        topappName--;
    }
    while (topappName < buf.data() + buf.size() && *topappName != ':') {
        topappName++;
    }
    topappName++;

    char *end = std::min(strchr(topappName, '/'), strchr(topappName, ':'));
    if (end == nullptr) {
        return {};
    }
    *end = '\0';
    return topappName;
}

std::string GetTopAppNameDumpsysAndroid10(void) {
    // matisse:/ # dumpsys activity lru | grep TOP
    // #101: fg     TOP  LCMN 21639:com.hengye.share/u0a351 act:activities|recents
    // #87: fg     BTOP ---N 4257:com.miui.securitycenter.remote/1000
    // #71: vis    BTOP ---N 6167:com.miui.securitycenter.bootaware/1000
    std::string buf;
    ExecCmdSync(&buf, "/system/bin/dumpsys", "activity", "lru");
    if (buf.empty()) {
        return {};
    }

    constexpr char TA_KEYWORD[] = " TOP";
    char *topappName = strstr(buf.data(), TA_KEYWORD);
    if (topappName == nullptr) {
        return {};
    }

    while (topappName < buf.data() + buf.size() && *topappName != ':') {
        topappName++;
    }
    topappName++;

    char *end = std::min(strchr(topappName, '/'), strchr(topappName, ':'));
    if (end == nullptr) {
        return {};
    }
    *end = '\0';
    return topappName;
}

std::string GetTopAppNameDumpsys(void) {
    auto ver = GetOSVersion();
    if (ver >= 10) {
        return GetTopAppNameDumpsysAndroid10(); // took ~40ms
    } else if (ver >= 7) {
        return GetTopAppNameDumpsysAndroid7(); // took ~100ms
    }
    return {};
}

std::string GetHomePackageName(void) {
    // Android 7+
    // /system/bin/cmd package resolve-activity -a android.intent.action.MAIN -c android.intent.category.HOME
    // Android 10+
    // /system/bin/pm resolve-activity -a android.intent.action.MAIN -c android.intent.category.HOME
    std::string buf;
    ExecCmdSync(&buf, "/system/bin/cmd", "package", "resolve-activity", "-a", "android.intent.action.MAIN", "-c",
                "android.intent.category.HOME");
    if (buf.empty()) {
        return {};
    }

    constexpr char KEYWORD[] = "packageName=";
    char *home = strstr(buf.data(), KEYWORD);
    if (home == nullptr) {
        return {};
    }

    home += sizeof(KEYWORD) - 1; // no '\0'
    char *end = strchr(home, '\n');
    if (end == nullptr) {
        return {};
    }

    *end = '\0';
    return home;
}

std::string GetTombstone(int pid) {
    constexpr char TOMBSTONE_DIR[] = "/data/tombstones";
    constexpr int TOMBSTONE_HEAD_LINES = 8;

    if (access(TOMBSTONE_DIR, F_OK) != 0) {
        return {};
    }

    char path[PATH_MAX];
    std::string latest;
    int64_t latestTs = 0;
    struct stat st;

    // get the latest tombstone
    DIR *tsfp = opendir(TOMBSTONE_DIR);
    struct dirent *dp;
    while ((dp = readdir(tsfp))) {
        // skip . and ..
        if (dp->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", TOMBSTONE_DIR, dp->d_name);
        if (stat(path, &st) == 0) {
            int64_t ts = st.st_mtim.tv_sec * 1e9 + st.st_mtim.tv_nsec;
            if (ts > latestTs) {
                latest = path;
                latestTs = ts;
            }
        }
    }
    closedir(tsfp);

    if (latestTs == 0) {
        return {};
    }

    FILE *fp = fopen(latest.c_str(), "re");
    if (fp == NULL) {
        return {};
    }

    char buf[512];
    bool isThisFile = false;
    for (int i = 0; i < TOMBSTONE_HEAD_LINES; ++i) {
        if (feof(fp))
            break;
        // pid: 17096, tid: 17098, name: unwind  >>> ./unwind <<<
        int owner_pid = 0;
        fgets(buf, sizeof(buf), fp);
        sscanf(buf, "%*s %d", &owner_pid);
        if (owner_pid == pid) {
            isThisFile = true;
            break;
        }
    }
    if (isThisFile == false) {
        fclose(fp);
        return {};
    }

    std::string bt;
    rewind(fp);
    while (feof(fp) == false) {
        fgets(buf, sizeof(buf), fp);
        bt += buf;
    }

    fclose(fp);
    return bt;
}

int GetScreenBrightness(void) {
    std::string buf;
    ExecCmdSync(&buf, "/system/bin/cmd", "settings", "get", "system", "screen_brightness");
    if (buf.empty()) {
        return -1;
    }

    int brightness = 0;
    if (ParseInt(buf.c_str(), &brightness)) {
        return brightness;
    }
    return -1;
}

static bool ParseIntStrict(const std::string &s, int *out) {
    if (s.empty()) {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    long val = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || errno == ERANGE || val < INT_MIN || val > INT_MAX) {
        return false;
    }
    *out = static_cast<int>(val);
    return true;
}

bool IsValidPeakRefreshRateValue(const std::string &hz) {
    int val = 0;
    return ParseIntStrict(hz, &val) && (val == -1 || val >= 20);
}

bool IsValidSfBackdoorIdxValue(const std::string &idx) {
    int val = 0;
    return ParseIntStrict(idx, &val) && val >= -1 && val <= 16;
}

static std::string GetSettingsValue(const char *ns, const char *key) {
    std::string buf;
    ExecCmdSync(&buf, "/system/bin/cmd", "settings", "get", ns, key);
    while (buf.empty() == false && IsSpace(buf.back())) {
        buf.pop_back();
    }
    return buf;
}

// Synchronous settings put, returns the exit status of the settings command
static bool CallSettingsPut(const char *ns, const char *key, const char *val) {
    return ExecCmdSync(nullptr, "/system/bin/cmd", "settings", "put", ns, key, val) == 0;
}

// Put then read back, so ROMs that silently reject the write are detected
static bool CallSettingsPutVerified(const char *ns, const char *key, const std::string &val) {
    if (CallSettingsPut(ns, key, val.c_str()) == false) {
        SPDLOG_WARN("settings put {} {} {} failed", ns, key, val);
        return false;
    }
    auto readback = GetSettingsValue(ns, key);
    if (readback != val) {
        SPDLOG_WARN("settings {}.{} verify failed, expect '{}' but got '{}'", ns, key, val, readback);
        return false;
    }
    return true;
}

// service call prints "Result: Parcel(...)" on success, and an error text
// (or a Parcel wrapping an exception) when the transaction failed
static bool CallSurfaceflingerBackdoorChecked(const char *code, const char *arg) {
    std::string out;
    int status = ExecCmdSync(&out, "/system/bin/service", "call", "SurfaceFlinger", code, "i32", arg);
    if (status != 0) {
        SPDLOG_WARN("service call SurfaceFlinger {} {} exit {}", code, arg, status);
        return false;
    }
    if (out.find("Result: Parcel(") == std::string::npos || out.find("Error") != std::string::npos ||
        out.find("Exception") != std::string::npos) {
        SPDLOG_WARN("service call SurfaceFlinger {} {} rejected: {}", code, arg, out);
        return false;
    }
    return true;
}

bool SysPeakRefreshRate(const std::string &hz, bool force) {
    if (IsValidPeakRefreshRateValue(hz) == false) {
        SPDLOG_ERROR("Invalid peak refresh rate '{}'", hz);
        return false;
    }

    // standard keys must be verified, they are the actual switch interface
    bool ok = CallSettingsPutVerified("system", "peak_refresh_rate", hz);
    ok = CallSettingsPutVerified("system", "min_refresh_rate", hz) && ok;

    // ROM private keys are best-effort: write them but never fail on them,
    // because they simply do not exist on non-MIUI devices
    CallSettingsPut("system", "miui_refresh_rate", hz.c_str());
    CallSettingsPut("secure", "miui_refresh_rate", hz.c_str());

    return ok;
}

bool SysSurfaceflingerBackdoor(const std::string &idx, bool force) {
    if (IsValidSfBackdoorIdxValue(idx) == false) {
        SPDLOG_ERROR("Invalid surfaceflinger backdoor index '{}'", idx);
        return false;
    }

    // >= Android 10
    // 1035 -1/0/1/2: setActiveConfig
    bool ok = CallSurfaceflingerBackdoorChecked("1035", idx.c_str());

    if (force) {
        // >= Android 11
        // 1036 1: Frame rate flexibility token acquired. count=1
        // 1036 0: Frame rate flexibility token released. count=0
        // service call SurfaceFlinger 1035 i32 -1 -- okay
        // service call SurfaceFlinger 1036 i32 1
        // service call SurfaceFlinger 1035 i32 2 -- not working
        ok = CallSurfaceflingerBackdoorChecked("1036", "1") && ok;
        ok = CallSurfaceflingerBackdoorChecked("1035", "-1") && ok;
        ok = CallSurfaceflingerBackdoorChecked("1036", "0") && ok;
        ok = CallSurfaceflingerBackdoorChecked("1035", idx.c_str()) && ok;
    }
    return ok;
}
