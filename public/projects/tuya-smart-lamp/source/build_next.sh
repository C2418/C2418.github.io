#!/bin/bash
#
# 自动递增版本并编译当前应用
#
# 使用方法：
#   cd /path/to/TuyaOS/apps/9999
#   ./build_next.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_NAME="9999"
TARGET_PLATFORM="bk7231n"
REL_APP_PATH="apps/9999"
VENDOR_DIR="$SCRIPT_DIR/../../vendor/bk7231n/tuyaos"
BUILD_SCRIPT="$VENDOR_DIR/build.sh"
OUTPUT_DIR="$SCRIPT_DIR/output"

if [[ ! -x "$BUILD_SCRIPT" ]]; then
    echo "Error: build script not found or not executable: $BUILD_SCRIPT" >&2
    exit 1
fi

if [[ ! -d "$OUTPUT_DIR" ]]; then
    mkdir -p "$OUTPUT_DIR"
fi

latest_version="$(
    find "$OUTPUT_DIR" -maxdepth 1 -mindepth 1 -type d -printf '%f\n' \
        | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' \
        | sort -V \
        | tail -n1
)"

if [[ -z "${latest_version}" ]]; then
    next_version="1.0.0"
else
    IFS='.' read -r major minor patch <<<"${latest_version}"
    patch=$((patch + 1))
    next_version="${major}.${minor}.${patch}"
fi

echo "Latest version     : ${latest_version:-<none>}"
echo "Next build version : ${next_version}"
echo "Invoking build..."

pushd "$VENDOR_DIR" >/dev/null
"$BUILD_SCRIPT" "$APP_NAME" "$next_version" "$TARGET_PLATFORM" "$REL_APP_PATH"
popd >/dev/null

echo "Build finished. Output directory:"
echo "  $OUTPUT_DIR/${next_version}"

