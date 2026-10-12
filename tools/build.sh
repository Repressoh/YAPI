#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${PS5_PAYLOAD_SDK:?Set PS5_PAYLOAD_SDK to an installed ps5-payload-dev SDK}"
test -f "$PS5_PAYLOAD_SDK/toolchain/prospero.mk"
mkdir -p dist

for variant in debug release; do
    debug=0
    if [[ "$variant" == debug ]]; then debug=1; fi
    make test-host DEBUG="$debug" TEST_DIR="$PWD/build_tests/$variant" HOSTCC="${HOSTCC:-clang-18}"
    payload="dist/yapi-$variant.elf"
    make -B DEBUG="$debug" TARGET="$payload"
    bash tools/verify-payload.sh "$variant" "$payload"
done

(cd dist && sha256sum yapi-debug.elf yapi-release.elf > SHA256SUMS.txt)
