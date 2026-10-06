#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# inventory.sh — capture everything needed to choose exact install commands.
#
# READ-ONLY. Installs nothing, loads nothing, changes nothing. Run it on the
# target before any package is touched, and send the output.
#
#   ./tools/inventory.sh > inventory.txt 2>&1
#
# A few probes need root to be complete (dmidecode, some ethtool fields,
# tcpdump). They are marked and skipped cleanly when unavailable.
# ---------------------------------------------------------------------------
set -u

hdr() { printf '\n========== %s ==========\n' "$1"; }
run() { printf '\n$ %s\n' "$*"; "$@" 2>&1 || printf '(command failed: rc=%d)\n' "$?"; }
have() { command -v "$1" >/dev/null 2>&1; }

printf 'inventory generated: %s\n' "$(date -Is)"
printf 'host: %s\n' "$(hostname)"

hdr "OS / KERNEL / TOOLCHAIN"
run cat /etc/os-release
run uname -a
run uname -r
have gcc     && run gcc --version
have g++     && run g++ --version
have cmake   && run cmake --version
have pkg-config && run pkg-config --version
have python3 && run python3 --version
have pip3    && run pip3 --version
printf '\nsecure boot (blocks unsigned kernel modules):\n'
have mokutil && mokutil --sb-state 2>&1 || printf '(mokutil not installed)\n'

hdr "PCI INVENTORY"
run lspci -nn
printf '\n-- GPU candidates --\n'
lspci -nn | grep -i -E 'nvidia|vga|3d' || printf '(none)\n'
printf '\n-- Mellanox / ConnectX candidates --\n'
lspci -nn | grep -i -E 'mellanox|connectx' || printf '(none)\n'

hdr "GPU DETAIL"
if have nvidia-smi; then
  run nvidia-smi
  run nvidia-smi --query-gpu=name,driver_version,memory.total,compute_cap,pcie.link.gen.current,pcie.link.width.current --format=csv
else
  printf '(nvidia-smi not installed)\n'
fi
[ -r /proc/driver/nvidia/version ] && run cat /proc/driver/nvidia/version
have nvcc && run nvcc --version || printf '\n(nvcc not installed — CUDA toolkit absent)\n'
for bdf in $(lspci -D 2>/dev/null | grep -i -E 'nvidia' | cut -d' ' -f1); do
  printf '\n-- lspci -vv for GPU %s (link speed/width) --\n' "$bdf"
  lspci -vv -s "$bdf" 2>/dev/null | grep -E 'LnkCap|LnkSta|Region' || printf '(needs root)\n'
done

hdr "NETWORK INTERFACES"
run ip -br link
run ip -br addr
for i in $(ls /sys/class/net 2>/dev/null); do
  [ "$i" = lo ] && continue
  vend=$(cat "/sys/class/net/$i/device/vendor" 2>/dev/null || echo -)
  printf '\n-- %s (PCI vendor %s) --\n' "$i" "$vend"
  # 0x15b3 = Mellanox
  if [ "$vend" = "0x15b3" ]; then printf '   ** MELLANOX **\n'; fi
  have ethtool && ethtool "$i" 2>/dev/null | grep -E 'Speed|Duplex|Link detected'
  have ethtool && ethtool -i "$i" 2>/dev/null | grep -E 'driver|version|firmware|bus-info'
  printf '   mtu: %s  operstate: %s\n' \
    "$(cat /sys/class/net/$i/mtu 2>/dev/null)" \
    "$(cat /sys/class/net/$i/operstate 2>/dev/null)"
done

hdr "RDMA STACK"
for t in ibv_devices ibv_devinfo rdma ibstat show_gids; do
  if have $t; then printf '%-14s : %s\n' "$t" "$(command -v $t)"; else printf '%-14s : NOT INSTALLED\n' "$t"; fi
done
have ibv_devices && run ibv_devices
have ibv_devinfo && run ibv_devinfo -v
have rdma && run rdma link show
printf '\n-- sysfs RDMA <-> netdev mapping (authoritative) --\n'
if [ -d /sys/class/infiniband ]; then
  for d in /sys/class/infiniband/*; do
    [ -e "$d" ] || continue
    dev=$(basename "$d")
    net=$(ls "$d/device/net" 2>/dev/null | head -1)
    fw=$(cat "$d/fw_ver" 2>/dev/null)
    printf '%s -> netdev=%s fw=%s\n' "$dev" "${net:-none}" "${fw:--}"
    for p in "$d"/ports/*; do
      [ -e "$p" ] || continue
      printf '   port %s: link_layer=%s state=%s rate=%s\n' \
        "$(basename $p)" \
        "$(cat $p/link_layer 2>/dev/null)" \
        "$(cat $p/state 2>/dev/null)" \
        "$(cat $p/rate 2>/dev/null)"
      printf '     GID types: '
      cat "$p"/gid_attrs/types/* 2>/dev/null | sort -u | tr '\n' ' '
      printf '\n'
    done
  done
else
  printf '/sys/class/infiniband does not exist — no RDMA stack loaded\n'
fi

hdr "KERNEL MODULES"
lsmod | grep -E '^(nvidia|nvidia_peermem|nv_peer_mem|mlx5_core|mlx5_ib|ib_core|ib_uverbs|rdma_cm|rdma_ucm)' \
  || printf '(none of the expected modules are loaded)\n'
printf '\n-- modinfo nvidia_peermem --\n'
modinfo nvidia_peermem 2>&1 | head -5 || printf '(not available)\n'

hdr "DMESG (nvidia / mlx5 / peermem)"
dmesg 2>/dev/null | grep -i -E 'nvidia|peermem|mlx5|gpudirect' | tail -40 \
  || printf '(dmesg needs root, or no matching lines)\n'

hdr "SDKs"
printf 'Rivermax : '; ls /usr/lib/x86_64-linux-gnu/librivermax.so* /opt/mellanox/rivermax/lib/librivermax.so* 2>/dev/null || printf 'NOT INSTALLED\n'
printf 'Holoscan : '; python3 -c 'import holoscan;print(holoscan.__version__)' 2>/dev/null || ls /opt/nvidia/holoscan 2>/dev/null || printf 'NOT INSTALLED\n'
for m in numpy scipy cupy pycuda cuda; do
  printf '%-8s : ' "$m"
  python3 -c "import $m,sys;sys.stdout.write(getattr($m,'__version__','(no __version__)')+'\n')" 2>/dev/null \
    || printf 'NOT INSTALLED\n'
done

hdr "PERFTEST"
for t in ib_write_bw ib_send_bw ib_write_lat; do
  if have $t; then printf '%-14s : %s\n' "$t" "$(command -v $t)"; else printf '%-14s : NOT INSTALLED\n' "$t"; fi
done
have ib_write_bw && (ib_write_bw --help 2>&1 | grep -q use_cuda \
  && printf 'ib_write_bw --use_cuda : SUPPORTED (perftest built with CUDA)\n' \
  || printf 'ib_write_bw --use_cuda : NOT supported (perftest built without CUDA)\n')

hdr "IOMMU / PCIe TOPOLOGY (affects GPUDirect)"
printf 'cmdline: %s\n' "$(cat /proc/cmdline)"
have nvidia-smi && run nvidia-smi topo -m

hdr "END"
printf 'inventory complete\n'
