#!/usr/bin/env bash
# Back up the full 16 MiB flash of an M5Stack StopWatch (ESP32-S3).
#
# The native USB-Serial/JTAG port drops long transfers ("Packet content
# transfer stopped") when reading the whole flash in one go with the stub, so
# read in 1 MiB chunks without the stub and retry each chunk. Same approach as
# ochyai/m5stack-stopwatch-simulator/scripts/backup-factory.sh.
#
# Usage: scripts/backup-factory.sh [/dev/cu.usbmodemXXXX]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
port="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[[ -n "${port}" ]] || { echo "error: no /dev/cu.usbmodem* port found" >&2; exit 1; }

backup_dir="${repo_root}/backup"
stamp="$(date +%Y%m%dT%H%M%S)"
image="${backup_dir}/factory-flash-16MB-${stamp}.bin"
chunk_dir="${backup_dir}/.chunks-${stamp}"
chunk_size=$((0x100000))
chunk_count=16

mkdir -p "${chunk_dir}"
trap 'rm -rf "${chunk_dir}"' EXIT

echo "Reading 16 MiB from ${port} in ${chunk_count} chunks..."
for ((i = 0; i < chunk_count; i++)); do
  offset=$((i * chunk_size))
  chunk="${chunk_dir}/chunk-$(printf '%02d' "$i").bin"
  for attempt in 1 2 3; do
    if esptool --chip esp32s3 --port "${port}" --no-stub \
        read-flash --flash-size 16MB --no-progress \
        "$(printf '0x%X' "${offset}")" "${chunk_size}" "${chunk}" >/dev/null 2>&1 \
       && [[ "$(wc -c <"${chunk}" | tr -d ' ')" == "${chunk_size}" ]]; then
      printf '[%02d/%02d] 0x%07X ok\n' "$((i + 1))" "${chunk_count}" "${offset}"
      break
    fi
    rm -f "${chunk}"
    [[ "${attempt}" -lt 3 ]] || { echo "error: chunk ${i} failed 3 times" >&2; exit 1; }
    echo "  retry chunk ${i} (${attempt}/3)"; sleep 1
  done
done

cat "${chunk_dir}"/chunk-*.bin >"${image}"
size="$(wc -c <"${image}" | tr -d ' ')"
[[ "${size}" == "16777216" ]] || { echo "error: got ${size} bytes" >&2; exit 1; }
shasum -a 256 "${image}" | tee "${image}.sha256"
echo "Backup complete: ${image}"
