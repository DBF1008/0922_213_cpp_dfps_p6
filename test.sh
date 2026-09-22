#!/system/bin/sh
#
# dfps 刷新率切换后端「尝试切换 -> 校验结果 -> 回退/降级」回归测试
# Regression tests for dfps refresh-rate backend verify & fallback chain
#
# !!! 本脚本需要在已 root 的 Android 设备上手动执行 !!!
# 用法 / Usage:
#   adb push test.sh /data/local/tmp/test.sh
#   adb push build/aarch64-linux-android23-clang/runnable/dfps /data/local/tmp/dfps
#   adb shell
#   su -c 'sh /data/local/tmp/test.sh /data/local/tmp/dfps'
#
# 不传参数时默认依次查找 ./dfps 与 /data/local/tmp/dfps
#

WORKDIR="/data/local/tmp/dfps_test"
DFPS_BIN="${1:-}"
PASS_CNT=0
FAIL_CNT=0
SKIP_CNT=0

log() { echo "[test] $*"; }

pass() { PASS_CNT=$((PASS_CNT + 1)); echo "  PASS: $1"; }
fail() { FAIL_CNT=$((FAIL_CNT + 1)); echo "  FAIL: $1"; }
skip() { SKIP_CNT=$((SKIP_CNT + 1)); echo "  SKIP: $1"; }

find_dfps() {
    if [ -n "$DFPS_BIN" ]; then
        [ -e "$DFPS_BIN" ] && [ ! -x "$DFPS_BIN" ] && chmod 755 "$DFPS_BIN" 2>/dev/null
        return
    fi
    for p in ./dfps /data/local/tmp/dfps; do
        [ -e "$p" ] && [ ! -x "$p" ] && chmod 755 "$p" 2>/dev/null
        if [ -x "$p" ]; then
            DFPS_BIN="$p"
            return
        fi
    done
    echo "FATAL: dfps binary not found, pass its path as arg1" >&2
    exit 1
}

is_root() { [ "$(id -u)" = "0" ]; }

can_su_shell() {
    is_root && su shell -c 'id -u' 2>/dev/null | grep -q '^2000$'
}

stop_dfps() {
    pkill -f dfps 2>/dev/null
    sleep 1
}

# $1: config content, $2: "root" | "shell"
start_dfps() {
    stop_dfps
    mkdir -p "$WORKDIR"
    rm -f "$WORKDIR/notify" "$WORKDIR/log"
    echo "$1" >"$WORKDIR/dfps.txt"
    chmod 644 "$WORKDIR/dfps.txt"
    if [ "$2" = "shell" ]; then
        su shell -c "$DFPS_BIN $WORKDIR/dfps.txt -o $WORKDIR/log -n $WORKDIR/notify"
    else
        "$DFPS_BIN" "$WORKDIR/dfps.txt" -o "$WORKDIR/log" -n "$WORKDIR/notify"
    fi
}

# $1: file, $2: timeout seconds; wait until file exists and non-empty
wait_file() {
    i=0
    while [ "$i" -lt "$2" ]; do
        [ -s "$1" ] && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# $1: pattern, $2: timeout seconds; wait until log contains pattern
wait_log() {
    i=0
    while [ "$i" -lt "$2" ]; do
        grep -q "$1" "$WORKDIR/log" 2>/dev/null && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

#-----------------------------------------------------------------------
# 单元 1: settings 写入-回读校验原语在本机可用
#-----------------------------------------------------------------------
test_settings_readback_primitive() {
    log "test_settings_readback_primitive"
    is_root || { skip "need root"; return; }

    old=$(settings get system peak_refresh_rate)
    settings put system peak_refresh_rate 60
    got=$(settings get system peak_refresh_rate)
    # restore
    settings put system peak_refresh_rate "${old:-0.0}" >/dev/null 2>&1

    if [ "$got" = "60" ] || [ "$got" = "60.0" ]; then
        pass "settings put/get roundtrip works"
    else
        fail "settings readback mismatch, got '$got'"
    fi
}

#-----------------------------------------------------------------------
# 单元 2: SurfaceFlinger backdoor 调用结果可解析（Result: Parcel 且无错误）
#-----------------------------------------------------------------------
test_sf_backdoor_result_primitive() {
    log "test_sf_backdoor_result_primitive"
    is_root || { skip "need root"; return; }

    out=$(service call SurfaceFlinger 1035 i32 -1 2>&1)
    echo "$out" | grep -q "Result: Parcel(" || { fail "no Parcel result: $out"; return; }
    echo "$out" | grep -Eq "Error|Exception" && { fail "error in result: $out"; return; }
    pass "service call SurfaceFlinger 1035 returns clean Parcel"
}

#-----------------------------------------------------------------------
# 单元 3: PEAK_REFRESH_RATE 后端切换成功且系统设置与通知文件一致
#-----------------------------------------------------------------------
test_peak_backend_success() {
    log "test_peak_backend_success"
    is_root || { skip "need root"; return; }

    start_dfps "/useSfBackdoor 0
- -1 -1
* 60 120" root

    wait_file "$WORKDIR/notify" 20 || { fail "notify file not written in 20s"; stop_dfps; return; }
    hz=$(cat "$WORKDIR/notify")
    sys=$(settings get system peak_refresh_rate)
    # 系统可能以浮点形式存储
    sys_int=$(echo "$sys" | cut -d. -f1)
    if [ "$hz" = "$sys_int" ]; then
        pass "notify($hz) matches system peak_refresh_rate($sys)"
    else
        fail "notify($hz) != system peak_refresh_rate($sys)"
    fi
    grep -q "Failed to switch refresh rate" "$WORKDIR/log" &&
        fail "unexpected switch failure in log" || pass "no switch failure logged"
    stop_dfps
}

#-----------------------------------------------------------------------
# 单元 4: Surfaceflinger backdoor 后端切换成功（索引 0 必然存在）
#-----------------------------------------------------------------------
test_backdoor_backend_success() {
    log "test_backdoor_backend_success"
    is_root || { skip "need root"; return; }

    start_dfps "/useSfBackdoor 1
- -1 -1
* 0 0" root

    wait_file "$WORKDIR/notify" 20 || { fail "notify file not written in 20s"; stop_dfps; return; }
    hz=$(cat "$WORKDIR/notify")
    if [ "$hz" = "0" ]; then
        pass "notify file updated to backdoor index 0"
    else
        fail "notify=$hz, expect 0"
    fi
    grep -q "Failed to switch refresh rate" "$WORKDIR/log" &&
        fail "unexpected switch failure in log" || pass "no switch failure logged"
    stop_dfps
}

#-----------------------------------------------------------------------
# 单元 5(核心回归): 切换失败时不得伪装成功
# 非 root 下 service call 会被权限拒绝 -> 主后端校验失败
# 值 0 对 peak 后端非法 -> 无法降级 -> 不得更新通知文件，且日志记录失败
#-----------------------------------------------------------------------
test_failed_switch_no_fake_notify() {
    log "test_failed_switch_no_fake_notify"
    can_su_shell || { skip "need root with 'su shell' support"; return; }

    start_dfps "/useSfBackdoor 1
- -1 -1
* 0 0" shell

    if wait_log "Failed to switch refresh rate" 25; then
        pass "failure is logged instead of faked"
    else
        fail "no failure logged (service call unexpectedly succeeded as shell?)"
        stop_dfps
        return
    fi
    if [ -f "$WORKDIR/notify" ] && [ "$(cat "$WORKDIR/notify")" = "0" ]; then
        fail "notify file updated although switch failed"
    else
        pass "notify file untouched on failure"
    fi
    stop_dfps
}

#-----------------------------------------------------------------------
# 单元 6: 降级链路一致性
# 非 root 下 backdoor 失败，-1 对 peak 后端合法 -> 尝试降级
# 无论 ROM 是否接受 -1，通知文件与降级日志必须自洽
#-----------------------------------------------------------------------
test_fallback_consistency() {
    log "test_fallback_consistency"
    can_su_shell || { skip "need root with 'su shell' support"; return; }

    start_dfps "/useSfBackdoor 1
- -1 -1
* -1 -1" shell

    # 等待一次切换尝试结束（失败或降级成功）
    wait_log "Failed to switch refresh rate\|Fallback backend works" 25 ||
        { fail "no switch attempt observed in 25s"; stop_dfps; return; }

    if grep -q "Fallback backend works" "$WORKDIR/log"; then
        if [ -f "$WORKDIR/notify" ] && [ "$(cat "$WORKDIR/notify")" = "-1" ]; then
            pass "fallback succeeded and notify updated"
        else
            fail "fallback logged as working but notify not updated"
        fi
    else
        if [ -f "$WORKDIR/notify" ]; then
            fail "notify updated although fallback did not succeed"
        else
            pass "both backends failed and notify untouched"
        fi
    fi
    stop_dfps
}

#-----------------------------------------------------------------------
# 单元 7: 非法配置（值与后端模式不匹配）被拒绝，dfps 不启动
#-----------------------------------------------------------------------
test_invalid_config_rejected() {
    log "test_invalid_config_rejected"
    is_root || { skip "need root"; return; }

    start_dfps "/useSfBackdoor 0
- -1 -1
* 7 7" root

    if wait_log "invalid refresh rate values\|Config error" 10; then
        pass "invalid config rejected at load time"
    else
        fail "invalid config not rejected"
    fi
    stop_dfps
}

#-----------------------------------------------------------------------
main() {
    find_dfps
    log "dfps binary: $DFPS_BIN"
    log "workdir: $WORKDIR"
    mkdir -p "$WORKDIR"

    test_settings_readback_primitive
    test_sf_backdoor_result_primitive
    test_peak_backend_success
    test_backdoor_backend_success
    test_failed_switch_no_fake_notify
    test_fallback_consistency
    test_invalid_config_rejected

    stop_dfps
    echo "----------------------------------------"
    echo "RESULT: pass=$PASS_CNT fail=$FAIL_CNT skip=$SKIP_CNT"
    [ "$FAIL_CNT" -eq 0 ]
}

main
