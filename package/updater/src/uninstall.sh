#!/bin/bash
# uninstall.sh - remove the mods. Entry point of the removal .UPD, staged as
# usb_update.sh (the name the deck runs).
#
# Everything the mods added lives in one archive, pdj.tar.gz, on the overlay
# partition. Deleting it is the whole removal; the rootfs is never touched.
# Next boot comes up stock.
#
# The front-panel service mode does the same without a USB stick.
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

if [[ -z $UPDATE_MEDIA_DEVICE ]]; then
    echo "Couldn't locate update media"
    exit 1
fi

# The update media may be mounted read-only, and this script's exit status is
# what the caller reports. Try for writable, carry on without.
if mount -o remount,rw "$UPDATE_MEDIA_DEVICE" "$UPDATE_MEDIA_MOUNTPOINT" 2>/dev/null; then
    MEDIA_WRITABLE=1
else
    MEDIA_WRITABLE=0
fi

# Fixed name: this is staged as usb_update.sh.
LOG_NAME=uninstall
if [[ "$MEDIA_WRITABLE" == 1 ]]; then
    exec >"$UPDATE_MEDIA_MOUNTPOINT"/"$LOG_NAME".log || true
    exec 2>&1
fi

trap 'printf "[%s] %s: %s\n" "$(cut -d" " -f1 /proc/uptime)" "$LOG_NAME" "${BASH_COMMAND:-}" 1>&2 || true' DEBUG

LANGUAGE=${2:-}

# MOD SETTINGS lives on the settings partition, not the overlay, so removing
# pdj.tar.gz never reaches it. Recovery does not mount that partition, so this
# does.
#
# Guarded on our own file being present, and deletes it by exact name: the
# deck's own CDJ3K_*.DAT files share the directory.
function remove_settings() {
    for dev in "$@"; do
        [[ -b "$dev" ]] || continue
        d=$(mktemp -d) || return 0
        if mount -o rw "$dev" "$d" 2>/dev/null; then
            if [[ -e "$d"/CDJ3K_MODSETTINGS.DAT ]]; then
                rm -f "$d"/CDJ3K_MODSETTINGS.DAT \
                      "$d"/CDJ3K_MODSETTINGS.DAT.BAK \
                      "$d"/CDJ3K_MODSETTINGS.DAT.new
                sync
                echo "removed MOD SETTINGS from $dev"
            fi
            umount "$d" 2>/dev/null || true
        fi
        rmdir "$d" 2>/dev/null || true
    done
}

function remove_overlay() {
    OVERLAY_MEDIA=$1

    OVERLAY_MEDIA_MOUNTPOINT=$(mktemp -d)
    mount -o rw "$OVERLAY_MEDIA" "$OVERLAY_MEDIA_MOUNTPOINT"

    # pdj.tar.gz is the whole mods overlay. -f: already-stock is a no-op.
    rm -f "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz

    # Plus our logs, so going back to stock leaves nothing behind.
    rm -rf "$OVERLAY_MEDIA_MOUNTPOINT"/cdj3k-mods-logs

    umount "$OVERLAY_MEDIA_MOUNTPOINT"
}

if [[ -b /dev/mmcblk0p5 ]]; then
    # Renesas model. The settings partition is not known on this variant, so the
    # candidates are probed; one that does not hold our file is left untouched.
    remove_overlay /dev/mmcblk0p5
    remove_settings /dev/mmcblk0p4 /dev/mmcblk0p6 /dev/mmcblk0p7
    gui_image D007 "$LANGUAGE" "" >/dev/null 2>&1
elif [[ -b /dev/mmcblk1p8 ]]; then
    # Rockchip model
    remove_overlay /dev/mmcblk1p8
    remove_settings /dev/mmcblk1p7
    pkill gui_image || true
    gui_image D007 "$LANGUAGE" >/dev/null 2>&1 &
else
    # Unrecognised deck; nothing removed. Show why and hold it there.
    echo "Unknown MMC partition layout"
    pkill gui_image 2>/dev/null || true
    gui_image D003 "$LANGUAGE" >/dev/null 2>&1 &
    trap - DEBUG
    while true; do sleep 1; done
fi
