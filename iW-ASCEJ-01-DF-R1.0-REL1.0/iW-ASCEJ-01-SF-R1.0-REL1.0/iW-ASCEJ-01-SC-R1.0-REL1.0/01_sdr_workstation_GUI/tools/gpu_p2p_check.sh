#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# gpu_p2p_check.sh — verify every prerequisite of the GPU / peer-to-peer path.
#
# "Peer to peer" here means GPUDirect RDMA: the Mellanox NIC DMAs payloads
# straight into GPU VRAM across PCIe, never touching system RAM. That needs
# the nvidia_peermem kernel module, which lets the RDMA stack register GPU
# memory. The GUI does not "turn P2P on" — rdma_rx_gpu registers VRAM with the
# NIC, and the GUI opens that VRAM through CUDA IPC.
#
# Each check prints PASS/FAIL and, on failure, the command that fixes it.
# ---------------------------------------------------------------------------
set -u
pass=0; fail=0
ok()   { printf '  \033[32m[PASS]\033[0m %s\n' "$*"; pass=$((pass+1)); }
bad()  { printf '  \033[31m[FAIL]\033[0m %s\n' "$*"; fail=$((fail+1)); }
fix()  { printf '         fix: %s\n' "$*"; }
info() { printf '  \033[36m[info]\033[0m %s\n' "$*"; }

echo "GPU / GPUDirect peer-to-peer pre-flight"
echo

echo "1. GPU and driver"
if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L >/dev/null 2>&1; then
    ok "$(nvidia-smi -L | head -1)"
else
    bad "nvidia-smi cannot see a GPU"; fix "install the NVIDIA driver"
fi

echo
echo "2. CUDA toolkit (needed to BUILD the GPU path)"
if command -v nvcc >/dev/null 2>&1; then
    ok "nvcc $(nvcc --version | sed -n 's/.*release \([0-9.]*\).*/\1/p')"
else
    bad "nvcc not found — the GUI will build CPU-only"
    fix "sudo apt install nvidia-cuda-toolkit"
fi

echo
echo "3. GPUDirect RDMA — the peer-to-peer module"
# /proc/modules rather than lsmod: lsmod is a separate package that may be
# absent, and a missing binary must not read as "module not loaded".
if grep -qE '^nvidia_peermem ' /proc/modules 2>/dev/null; then
    ok "nvidia_peermem loaded (NIC -> GPU peer-to-peer enabled)"
elif grep -qE '^nv_peer_mem ' /proc/modules 2>/dev/null; then
    ok "nv_peer_mem loaded (legacy module; peer-to-peer enabled)"
else
    bad "no peer-memory module loaded — NIC cannot DMA into VRAM"
    fix "sudo modprobe nvidia_peermem"
    fix "to persist:  echo nvidia_peermem | sudo tee /etc/modules-load.d/nvidia-peermem.conf"
fi

echo
echo "4. RDMA device"
if command -v ibv_devices >/dev/null 2>&1; then
    n=$(ibv_devices 2>/dev/null | awk 'NR>2 && NF' | wc -l)
    if [ "$n" -gt 0 ]; then ok "$n RDMA device(s): $(ibv_devices | awk 'NR>2 && NF{print $1}' | tr '\n' ' ')"
    else bad "no RDMA devices"; fix "check the Mellanox NIC and MLNX_OFED / rdma-core"; fi
else
    bad "ibv_devices not installed"; fix "sudo apt install ibverbs-utils rdma-core"
fi

echo
echo "5. PCIe topology between NIC and GPU"
if command -v nvidia-smi >/dev/null 2>&1; then
    t=$(nvidia-smi topo -m 2>/dev/null | head -20)
    if [ -n "$t" ]; then
        info "nvidia-smi topo -m (look at the NIC row, GPU column):"
        echo "$t" | sed 's/^/         /'
        info "PIX / PXB = good peer-to-peer path.  SYS / NODE = crosses the"
        info "CPU root complex; P2P still works but at reduced bandwidth."
    fi
fi

echo
echo "6. The receiver's ring"
if [ -e /dev/shm/iqring ]; then
    m=$(head -c 8 /dev/shm/iqring | od -An -c | tr -d ' \n')
    case "$m" in
        *1GGNIRQI*|*IQRINGG1*) ok "/dev/shm/iqring is a GPUDirect ring (IQRINGG1)";;
        *10GNIRQI*|*IQRING01*) info "/dev/shm/iqring is a HOST ring (IQRING01) — CPU ingest."
                               info "Start rdma_rx_gpu (not rdma_rx) for the peer-to-peer path.";;
        *) bad "/dev/shm/iqring has an unrecognised magic";;
    esac
    owner=$(stat -c %U /dev/shm/iqring)
    me=$(id -un)
    if [ "$owner" = "$me" ]; then ok "ring owned by you ($me) — CUDA IPC will open"
    else
        bad "ring owned by '$owner', you are '$me' — CUDA IPC WILL FAIL"
        fix "run rdma_rx_gpu and the GUI as the SAME user"
    fi
else
    info "/dev/shm/iqring absent — start rdma_rx_gpu (or tools/iqring_sim to test the GUI)"
fi

echo
echo "7. Running as root?"
if [ "$(id -u)" -eq 0 ]; then
    bad "you are root — if rdma_rx_gpu is not also root, CUDA IPC fails"
    fix "run the GUI without sudo; grant PCIe BAR access instead with:"
    fix "  sudo setcap cap_sys_rawio+ep ./build/sdr_workstation"
else
    ok "not root"
fi

echo
echo "---------------------------------------------"
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ] && echo "  Ready for the GPU / peer-to-peer path." \
                  || echo "  Fix the FAIL items above, then re-run."
