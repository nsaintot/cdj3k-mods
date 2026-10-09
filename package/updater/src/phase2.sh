#!/bin/bash
# phase2.sh - installer step 2 of 2. Runs at the second boot as the shadowed
# application. Removes its own shadow (the overlay's pdj/), then installs:
#
#     scripts/apl_start.sh  <-  payload.sh  +  (stock apl_start.sh appended)
#
# The binaries live in mods/, never under pdj/, so `rm -r pdj` leaves them.
set -euTo pipefail

UPDATE_MEDIA_DEVICE=
UPDATE_MEDIA_MOUNTPOINT=

while read -r MOUNT; do
    DEVICE=$(echo "$MOUNT" | cut -d' ' -f1)
    MOUNTPOINT=$(echo "$MOUNT" | cut -d' ' -f2)
    if [[ -e $MOUNTPOINT/CDJ3Kv000.UPD ]]; then
        UPDATE_MEDIA_DEVICE=$DEVICE
        UPDATE_MEDIA_MOUNTPOINT=$MOUNTPOINT
        break
    fi
done < /proc/mounts

OVERLAY_MEDIA_MOUNTPOINT=/mnt
MODS_LOG_DIR=$OVERLAY_MEDIA_MOUNTPOINT/cdj3k-mods-logs

# As phase1: prefer the stick, fall back to the overlay partition. Explicit
# name, since this runs as pdj/EP<n>.
LOG_NAME=phase2
LOG_DIR=

if [[ -n $UPDATE_MEDIA_DEVICE ]] &&
   mount -o remount,rw "$UPDATE_MEDIA_DEVICE" "$UPDATE_MEDIA_MOUNTPOINT" 2>/dev/null; then
    LOG_DIR=$UPDATE_MEDIA_MOUNTPOINT
elif mkdir -p "$MODS_LOG_DIR" 2>/dev/null; then
    LOG_DIR=$MODS_LOG_DIR
fi

if [[ -n $LOG_DIR ]]; then
    exec >"$LOG_DIR"/"$LOG_NAME".log || true
    exec 2>&1

    trap 'printf "[%s] %s: %s\n" "$(cut -d" " -f1 /proc/uptime)" "$LOG_NAME" "${BASH_COMMAND:-}" 1>&2 || true' DEBUG
fi

PDJ_TAR_WORKDIR=$(mktemp -d)
tar -xvzf "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz -C "$PDJ_TAR_WORKDIR"

rm -r "$PDJ_TAR_WORKDIR"/pdj

APL_START=$PDJ_TAR_WORKDIR/scripts/apl_start.sh

# Preload on the application line only, not exported.
#
# The launcher runs i2cget, sysctl, taskset, aplay, fw_printenv and more before
# the app, and picks one of three binaries by /tmp/testmode:
#
#   off -> ./EP<n>          the application   <- the only one we want
#   on1 -> ./EP<n>TestMode  service test mode <- must stay stock
#   on2 -> gdbserver attach
#
# Rewriting the one line keeps the preload on the application: `./EP122`
# matches, `./EP122TestMode` does not (anchored at end of line).
#
# BRE, not -E/-r: the deck's sed may be busybox.
STOCK_LAUNCHER=/home/root/scripts/apl_start.sh
APP_LINE='^[[:space:]]*\./EP[0-9][0-9]*[[:space:]]*$'
MATCHES=$(grep -c "$APP_LINE" "$STOCK_LAUNCHER" || true)

if [ "$MATCHES" = "1" ]; then
    mkdir -p "$(dirname "$APL_START")"
    mv "$PDJ_TAR_WORKDIR"/payload.sh "$APL_START"
    sed 's#^\([[:space:]]*\)\./\(EP[0-9][0-9]*\)[[:space:]]*$#\1LD_PRELOAD=/home/root/mods/ep122_shim.so ./\2#' \
        "$STOCK_LAUNCHER" >> "$APL_START"
    echo "phase2: preload targeted at the application line only"
else
    # All or nothing: an unrecognised launcher has no line we can be sure is
    # the application. Install nothing, keep the stock launcher, log why.
    rm -f "$PDJ_TAR_WORKDIR"/payload.sh
    rm -rf "$PDJ_TAR_WORKDIR"/mods
    echo "phase2: launcher has $MATCHES application lines, expected 1"
    echo "phase2: unsupported launcher - mods NOT installed, deck stays stock"
fi

# Write-then-rename. This write installs the runtime launcher: a truncated
# apl_start.sh would run to EOF and exit 0: no app, no error.
tar -czf "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz.new -C "$PDJ_TAR_WORKDIR" .
sync
mv "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz.new "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz
sync

if [[ -b /dev/mmcblk1p8 ]]; then
    # Rockchip model
    systemctl reboot
    exit 0
fi

systemctl disable xserver-nodm
systemctl stop xserver-nodm
echo 1 > /sys/class/vtconsole/vtcon1/bind
openvt -s -- echo '*** Phase 2 of 2 complete. Please manually power cycle your CDJ. ***'
sleep 30d
