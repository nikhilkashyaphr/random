#!/usr/bin/env bash
# 05_gpudirect_setup.sh -- prepare the box for the GPUDirect receiver
# (rdma_rx_gpu): NIC DMAs payloads straight into A4000 memory.
#
# Does three things, then verifies:
#   1. loads nvidia-peermem (lets ibv_reg_mr accept cudaMalloc'd memory)
#   2. puts $SUDO_USER in the 'rdma' group so rdma_rx_gpu can run WITHOUT
#      sudo -- required because CUDA IPC only works between processes of
#      the SAME user, and the visualizer runs unprivileged
#   3. raises the memlock limit permanently (RDMA pins memory)
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }
U="${SUDO_USER:-$USER}"

echo "== 1. nvidia-peermem =="
if modprobe nvidia-peermem 2>/dev/null; then
    echo "loaded."
    echo nvidia-peermem > /etc/modules-load.d/nvidia-peermem.conf
    echo "(persisted via /etc/modules-load.d)"
else
    echo "FAILED to load nvidia-peermem."
    echo "  - NVIDIA driver >= 470 ships it; check: dkms status, nvidia-smi"
    echo "  - With DOCA-OFED the module may be named nv_peer_mem instead."
    exit 1
fi
lsmod | grep -E "peermem|nv_peer" || true

echo
echo "== 2. unprivileged RDMA for $U =="
getent group rdma >/dev/null || groupadd rdma
usermod -aG rdma "$U"
echo "$U added to 'rdma' group (re-login for it to take effect)."
ls -l /dev/infiniband/uverbs* || true
echo "(if uverbs devices are not group 'rdma', rdma-core's udev rules will"
echo " fix that on reboot, or: chgrp rdma /dev/infiniband/uverbs*; chmod 660 ...)"

echo
echo "== 3. memlock limit =="
LIM=/etc/security/limits.d/rdma-memlock.conf
printf "* soft memlock unlimited\n* hard memlock unlimited\n" > "$LIM"
echo "wrote $LIM (re-login required). Current shell: ulimit -l = $(ulimit -l)"

echo
echo "== 4. PCIe peer-to-peer sanity =="
echo "GPU and NIC slots (ideally under the same root complex/switch):"
lspci -tv | grep -E "Mellanox|NVIDIA" || true
echo "If GPUDirect registration later fails or is slow, add 'iommu=pt' to"
echo "GRUB_CMDLINE_LINUX in /etc/default/grub, update-grub, reboot."

echo
echo "Done. Build and run (as $U, after re-login):"
echo "   make gpu"
echo "   ./rdma_rx_gpu -a 192.168.100.2          # note: NO sudo"
echo "   python viz/holoscan_iq_viz.py            # auto-detects GPU ring"
