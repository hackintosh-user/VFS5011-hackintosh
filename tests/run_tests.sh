#!/bin/bash
# run_tests.sh
#
# Builds and runs the standalone unit tests in test_matcher.c against
# shared/sen/hack-touchid-matcher.c + NBIS. No USB, no CoreFoundation, no sensor --
# safe to run in CI (or locally) with no hardware attached.
#
# Run from anywhere; paths are resolved relative to this script.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "==> Building test_matcher..."
clang "$SCRIPT_DIR/test_matcher.c" "$ROOT_DIR/shared/sen/hack-touchid-matcher.c" \
    "$ROOT_DIR"/NBIS/mindtct/*.c "$ROOT_DIR"/NBIS/bozorth3/*.c \
    -o "$SCRIPT_DIR/test_matcher" \
    -I"$ROOT_DIR/shared/sen" -I"$ROOT_DIR/NBIS/include" \
    -lm \
    -Wno-implicit-function-declaration

echo "==> Running test_matcher..."
"$SCRIPT_DIR/test_matcher"
