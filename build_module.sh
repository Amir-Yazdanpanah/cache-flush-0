#!/usr/bin/env bash
set -euo pipefail

KERNEL_BUILD_DIR="${1:?usage: build_module.sh KERNEL_BUILD_DIR OUTPUT CROSS_COMPILE}"
OUTPUT="${2:?usage: build_module.sh KERNEL_BUILD_DIR OUTPUT CROSS_COMPILE}"
CROSS_COMPILE="${3:-riscv64-linux-gnu-}"
MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

make -C "${KERNEL_BUILD_DIR}" \
  M="${MODULE_DIR}" \
  ARCH=riscv \
  CROSS_COMPILE="${CROSS_COMPILE}" \
  modules

install -m 0644 "${MODULE_DIR}/cache_flush.ko" "${OUTPUT}"
