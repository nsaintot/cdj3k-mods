#!/bin/bash
# usb_update.sh - install entry point. The deck unpacks the update media and
# runs:  usb_update.sh <ISO_MOUNTPOINT> <LANGUAGE>
#
# Stages an overlay (pdj.tar.gz) on the application partition carrying the two
# binaries and a two-phase installer, then hands back for the reboot into
# phase1. The overlay root maps onto /home/root at boot, so mods/ lands at
# /home/root/mods/. Nothing touches the read-only rootfs; nothing is reflashed.
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
# what the caller reports. Try for writable; carry on without.
if mount -o remount,rw "$UPDATE_MEDIA_DEVICE" "$UPDATE_MEDIA_MOUNTPOINT" 2>/dev/null; then
    MEDIA_WRITABLE=1
else
    MEDIA_WRITABLE=0
fi

# Fixed name: the removal package is staged under this same filename.
LOG_NAME=install
# Log to tmpfs always, copy on exit to the application partition (persistent,
# readable at /mnt/cdj3k-mods-logs/) and to the stick when writable.
TMP_LOG=/tmp/cdj3k-$LOG_NAME.log
exec >"$TMP_LOG" 2>&1 || true

save_log() {
    rc=$?
    trap - DEBUG EXIT
    echo "[cdj3k-mods] $LOG_NAME finished, exit=$rc"
    if [[ "$MEDIA_WRITABLE" == 1 ]]; then
        cp "$TMP_LOG" "$UPDATE_MEDIA_MOUNTPOINT"/"$LOG_NAME".log 2>/dev/null || true
    fi
    # The application partition: whichever layout this deck has.
    for dev in /dev/mmcblk1p8 /dev/mmcblk0p5; do
        [[ -b "$dev" ]] || continue
        d=$(mktemp -d) || break
        if mount -o rw "$dev" "$d" 2>/dev/null; then
            mkdir -p "$d"/cdj3k-mods-logs 2>/dev/null \
                && cp "$TMP_LOG" "$d"/cdj3k-mods-logs/"$LOG_NAME".log 2>/dev/null || true
            sync; umount "$d" 2>/dev/null || true
        fi
        rmdir "$d" 2>/dev/null || true
        break
    done
    exit $rc
}
trap save_log EXIT

trap 'printf "[%s] %s: %s\n" "$(cut -d" " -f1 /proc/uptime)" "$LOG_NAME" "${BASH_COMMAND:-}" 1>&2 || true' DEBUG

ISO_MOUNTPOINT=$1
LANGUAGE=$2

function safe_cp() {
    SRC=$1
    DST=$2

    mkdir -p "$(dirname "$DST")"
    cp "$SRC" "$DST"
}

# gui_image never returns; it holds the screen until killed. Always
# `pkill gui_image`, then run it in the background.
#
# Fatal: show the reason and hang, so it stays on screen until power-cycle.
function mods_fail() {
    echo "$1"
    pkill gui_image 2>/dev/null || true
    gui_image D003 "$LANGUAGE" >/dev/null 2>&1 &
    led_start "$LED_ERROR"
    # DEBUG trap fires per command; an endless loop under it floods the log.
    trap - DEBUG
    while true; do sleep 1; done
}

# Rockchip only: GPIO 71
LED_GPIO=71
LED_WORKING="0.08 0.08 0.08 0.6"   # heartbeat: pulse-pulse-pause
LED_DONE="1 1"                     # slow
LED_ERROR="0.05 0.05"              # urgent flutter
LED_PID=
LED_READY=0

function led_init() {
    [[ -w /sys/class/gpio/export ]] || return 0
    echo "$LED_GPIO" > /sys/class/gpio/export 2>/dev/null || true
    echo out > /sys/class/gpio/gpio$LED_GPIO/direction 2>/dev/null || true
    [[ -w /sys/class/gpio/gpio$LED_GPIO/value ]] || return 0
    # Take the light over from any blinker already running.
    killall ledcontrol 2>/dev/null || true
    LED_READY=1
}

function led_start() {
    [[ "$LED_READY" == 1 ]] || return 0
    [[ -n "$LED_PID" ]] && { kill "$LED_PID" 2>/dev/null || true; }
    # stdio to /dev/null: an inherited log fd keeps the update media busy.
    # DEBUG trap off: the loop never ends.
    (
        trap - DEBUG
        while :; do
            state=1
            for d in $1; do
                echo "$state" > /sys/class/gpio/gpio$LED_GPIO/value 2>/dev/null || true
                sleep "$d"
                state=$(( 1 - state ))
            done
        done
    ) >/dev/null 2>&1 &
    LED_PID=$!
}

function install_phase1() {
    OVERLAY_MEDIA=$1

    OVERLAY_MEDIA_MOUNTPOINT=$(mktemp -d)
    ls "$OVERLAY_MEDIA_MOUNTPOINT"
    mount -o rw "$OVERLAY_MEDIA" "$OVERLAY_MEDIA_MOUNTPOINT"

    PDJ_TAR_WORKDIR=$(mktemp -d)

    # Built fresh, replacing whatever is there: everything in the overlay comes
    # from an update, so the last one flashed defines it.
    #
    # phase1 runs as the app launcher and hands off to phase2, which installs
    # payload.sh as the persistent apl_start.sh prefix.
    safe_cp "$ISO_MOUNTPOINT"/phase1.sh  "$PDJ_TAR_WORKDIR"/scripts/apl_start.sh
    safe_cp "$ISO_MOUNTPOINT"/phase2.sh  "$PDJ_TAR_WORKDIR"/phase2.sh
    safe_cp "$ISO_MOUNTPOINT"/payload.sh "$PDJ_TAR_WORKDIR"/payload.sh
    # Removal is its own package (build.sh --remove) or the front-panel combo.

    # Into mods/ at the overlay root -> /home/root/mods/. Both phases re-tar the
    # whole workdir, so these carry through untouched.
    mkdir -p "$PDJ_TAR_WORKDIR"/mods
    safe_cp "$ISO_MOUNTPOINT"/mods/ep122_shim.so "$PDJ_TAR_WORKDIR"/mods/ep122_shim.so
    safe_cp "$ISO_MOUNTPOINT"/mods/stemd_client  "$PDJ_TAR_WORKDIR"/mods/stemd_client
    chmod 0755 "$PDJ_TAR_WORKDIR"/mods/stemd_client

    # authorized_keys beside the .UPD turns on root SSH; absent by default.
    # 700/600 are what sshd requires. payload.sh starts the daemons when present.
    if [[ -e "$UPDATE_MEDIA_MOUNTPOINT"/authorized_keys ]]; then
        safe_cp "$UPDATE_MEDIA_MOUNTPOINT"/authorized_keys "$PDJ_TAR_WORKDIR"/.ssh/authorized_keys
        chmod 700 "$PDJ_TAR_WORKDIR"/.ssh
        chmod 600 "$PDJ_TAR_WORKDIR"/.ssh/authorized_keys
    fi

    # Write-then-rename: rename(2) is atomic, so an interrupted write loses the
    # new archive, not the working one.
    tar -czf "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz.new -C "$PDJ_TAR_WORKDIR" .
    sync
    mv "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz.new "$OVERLAY_MEDIA_MOUNTPOINT"/pdj.tar.gz
    sync
    umount "$OVERLAY_MEDIA_MOUNTPOINT"
}

# The deck's firmware version, as u-boot holds it: <major>.<minor>, e.g. 3.22.
# The mods resolve against 3.13 and newer; below that they resolve nothing and
# the deck would flash an overlay that never activates.
#
# Only the floor is enforced. A version above the tested range still installs:
# the shim checks the firmware itself and installs all of its mods or none.
FW_RELEASE=$(fw_printenv -n release 2>/dev/null || true)
case "$FW_RELEASE" in
    [0-9]*.[0-9]*)
        FW_MAJOR=${FW_RELEASE%%.*}
        FW_MINOR=${FW_RELEASE#*.}
        if [[ "$FW_MAJOR" -lt 3 ]] || { [[ "$FW_MAJOR" -eq 3 ]] && [[ "$FW_MINOR" -lt 13 ]]; }; then
            mods_fail "Firmware $FW_RELEASE is too old. 3.13 or newer is required."
        fi
        echo "firmware $FW_RELEASE"
        ;;
    *)
        # Unreadable: carry on and let the shim's own check decide.
        echo "could not read the firmware version (got '$FW_RELEASE')"
        ;;
esac

if [[ -b /dev/mmcblk0p5 ]]; then
    # Renesas model
    install_phase1 /dev/mmcblk0p5
    gui_image D007 "$LANGUAGE" "" >/dev/null 2>&1
elif [[ -b /dev/mmcblk1p8 ]]; then
    # Rockchip model
    led_init
    led_start "$LED_WORKING"
    install_phase1 /dev/mmcblk1p8

    led_start "$LED_DONE"
    pkill gui_image || true
    gui_image D007 "$LANGUAGE" >/dev/null 2>&1 &
else
    # Neither layout matched.
    mods_fail "Unsupported model."
fi
