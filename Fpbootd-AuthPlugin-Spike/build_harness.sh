#!/bin/bash
#
# build_harness.sh
#
# Builds fpbootd_spike_test_harness.c -- a plain command-line tool,
# NOT a bundle. Run FpbootdSpike.bundle through it before ever
# registering the bundle anywhere near a real login.
#
# Usage:
#   ./build_harness.sh
#   ./fpbootd_spike_test_harness
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "Compiling fpbootd_spike_test_harness..."
clang fpbootd_spike_test_harness.c \
    -o fpbootd_spike_test_harness \
    -framework Security \
    -framework CoreFoundation \
    -Wall

echo ""
echo "Built: $SCRIPT_DIR/fpbootd_spike_test_harness"
echo ""
echo "Run it (make sure FpbootdSpike.bundle is already built):"
echo "  ./fpbootd_spike_test_harness"
echo ""
echo "Worth having a second terminal open first:"
echo "  tail -f /Library/Logs/fpbootd.log"
