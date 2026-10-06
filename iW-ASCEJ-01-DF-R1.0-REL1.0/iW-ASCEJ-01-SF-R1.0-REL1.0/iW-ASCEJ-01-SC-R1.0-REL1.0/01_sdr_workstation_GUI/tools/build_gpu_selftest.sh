#!/usr/bin/env bash
# Build the GPU self-test. Run from the GUI source root.
set -euo pipefail
nvcc -O2 -std=c++17 -DSDR_ENABLE_CUDA -Xcompiler -fPIC -Isrc/core \
     tools/gpu_selftest.cpp src/core/GpuFft.cpp src/core/GpuKernels.cu \
     $(pkg-config --cflags --libs Qt5Core) -lcufft -o gpu_selftest
echo "built ./gpu_selftest — run it, and watch nvtop while it runs"
