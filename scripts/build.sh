#!/usr/bin/env bash

set -euo pipefail

rm -rf build
cmake -S . -B build
cmake --build build --parallel 1 --verbose
