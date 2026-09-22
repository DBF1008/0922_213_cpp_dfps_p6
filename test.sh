#!/usr/bin/env bash
#
# 手动运行 dfps 全部单元测试（主机侧，无需 Android 设备/NDK）
# Manually build & run all dfps unit tests on the host.
#
# Usage:
#   ./test.sh            # build & run every tests/test_*.cpp
#   CXX=clang++ ./test.sh
#
set -u

BASEDIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$BASEDIR/build/host_tests"
CXX="${CXX:-c++}"
CXXFLAGS="-std=c++17 -Wall -Werror -I$BASEDIR/source -I$BASEDIR/tests"

shopt -s nullglob
TESTS=("$BASEDIR"/tests/test_*.cpp)
if [ ${#TESTS[@]} -eq 0 ]; then
    echo " ! no unit tests found under tests/"
    exit 1
fi

mkdir -p "$BUILD_DIR"

pass=0
fail=0
failed=()
for src in "${TESTS[@]}"; do
    name="$(basename "$src" .cpp)"
    bin="$BUILD_DIR/$name"
    echo ">>> Building $name"
    if ! $CXX $CXXFLAGS "$src" -o "$bin"; then
        echo "xxx $name: BUILD FAILED"
        fail=$((fail + 1))
        failed+=("$name(build)")
        continue
    fi
    echo ">>> Running  $name"
    if "$bin"; then
        echo "ok  $name"
        pass=$((pass + 1))
    else
        echo "xxx $name: FAILED"
        fail=$((fail + 1))
        failed+=("$name")
    fi
    echo
done

echo "======================================"
echo "unit tests: $pass passed, $fail failed"
if [ $fail -ne 0 ]; then
    echo "failed: ${failed[*]}"
    exit 1
fi
echo "all unit tests passed"
