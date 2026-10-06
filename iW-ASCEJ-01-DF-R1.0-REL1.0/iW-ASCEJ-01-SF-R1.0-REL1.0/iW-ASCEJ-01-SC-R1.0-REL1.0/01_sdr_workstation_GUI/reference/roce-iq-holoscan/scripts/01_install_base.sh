#!/usr/bin/env bash
# 01_install_base.sh -- base packages for the RoCEv2 IQ demo (Ubuntu 22.04)
#
# Installs the inbox RDMA userspace stack, build tools, diagnostics and the
# Vulkan loader Holoviz needs. Safe to re-run. NVIDIA driver/CUDA and the
# optional DOCA-OFED stack are covered in README.md (sections 3 and 4).
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y \
    build-essential git pkg-config \
    rdma-core ibverbs-providers ibverbs-utils rdmacm-utils \
    libibverbs-dev librdmacm-dev \
    perftest infiniband-diags \
    pciutils ethtool net-tools \
    python3-venv python3-pip \
    libvulkan1 vulkan-tools

echo
echo "== RDMA devices detected =="
rdma link || true
echo
echo "== ConnectX PCI status (check LnkSta width/speed) =="
lspci -d 15b3: -vvv 2>/dev/null | grep -E "ConnectX|LnkSta:" || \
    echo "No Mellanox device found -- is the card seated?"
echo
echo "Base install done. Next: NVIDIA driver + CUDA (README section 3),"
echo "then sudo ./scripts/02_setup_network.sh"
