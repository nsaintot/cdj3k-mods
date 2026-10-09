#!/bin/bash
# payload.sh - runtime. phase2 installs it as the prefix of
# /home/root/scripts/apl_start.sh, so it runs every boot before EP122 starts.
# Starts the STEMS sidecar and, with a key present, SSH. A service-mode boot
# removes the mods instead.
#
# Must never abort the boot: no `set -e`, no `exit`. Control has to reach the
# stock launcher appended below.

MODS_SHIM=/home/root/mods/ep122_shim.so
MODS_AGENT=/home/root/mods/stemd_client
MODS_SSH_KEYS=/home/root/.ssh/authorized_keys
MODS_LOG=/tmp/cdj3k-mods.log
# /tmp is tmpfs; the overlay partition survives the reboot. uninstall.sh clears it.
MODS_LOG_DIR=/mnt/cdj3k-mods-logs
MODS_LOG_KEEP=$MODS_LOG_DIR/runtime.log

# Best-effort logging: a failed write must not stop the boot.
mods_log() {
    echo "[cdj3k-mods] $*" >>"$MODS_LOG" 2>/dev/null || true
}

# Written by subucom_read from pre-setting.sh, which systemd orders before this
# service. "off" is a normal boot; anything else is the front-panel service mode.
MODS_TESTMODE=/tmp/testmode
MODS_SETTINGS=/home/root/settings/CDJ3K_MODSETTINGS
MODS_REMOVE=0
if [ -f "$MODS_TESTMODE" ] && [ "$(cat "$MODS_TESTMODE" 2>/dev/null)" != "off" ]; then
    MODS_REMOVE=1
fi

mods_log "boot at uptime $(cut -d' ' -f1 /proc/uptime 2>/dev/null)"

# 0) Service mode removes the mods. The overlay is the whole install, so deleting
#    it leaves the deck stock from the next boot. This boot carries on into
#    EP122TestMode, which never had the shim.
if [ "$MODS_REMOVE" = 1 ]; then
    mods_log "service mode ($(cat "$MODS_TESTMODE" 2>/dev/null)) - removing the mods"
    rm -f  /mnt/pdj.tar.gz      2>/dev/null || true
    rm -rf /mnt/cdj3k-mods-logs 2>/dev/null || true
    # MOD SETTINGS lives on the settings partition, not the overlay; it is
    # already mounted rw here. Exact names: the deck's own CDJ3K_*.DAT files
    # share the directory.
    for f in "$MODS_SETTINGS".DAT "$MODS_SETTINGS".DAT.BAK "$MODS_SETTINGS".DAT.new; do
        rm -f "$f" 2>/dev/null || true
    done
    sync
    mods_log "overlay and MOD SETTINGS removed - reboot normally for a stock deck"
fi

if [ "$MODS_REMOVE" = 0 ]; then

# 1) Shim. LD_PRELOAD is not set here: phase2 put it on the launcher's
#    application line so it reaches EP122 only, and exporting it here would undo
#    that. Verbose logging: add EP122_MOD_LOGLEVEL=debug on that line. The
#    sidecar's level is STEMD_LOGLEVEL, set in front of "$MODS_AGENT" where it
#    is launched below (there is no systemd unit for it); its output goes to
#    /tmp/stemd_client.log, not the journal.
if [ -f "$MODS_SHIM" ]; then
    mods_log "shim present at $MODS_SHIM (preloaded on the application line)"
else
    mods_log "shim missing at $MODS_SHIM - deck will run stock"
fi

# 2) STEMS sidecar, its own process. Binds /run/stemd-client.sock itself.
#    Backgrounded, so it survives this shell and the next boot finds it via
#    pidof.
if [ -x "$MODS_AGENT" ]; then
    if pidof stemd_client >/dev/null 2>&1; then
        mods_log "stemd_client already running"
    else
        "$MODS_AGENT" >/tmp/stemd_client.log 2>&1 &
        mods_log "started stemd_client (pid $!)"
    fi
elif [ -e "$MODS_AGENT" ]; then
    # Present, but not executable.
    mods_log "stemd_client at $MODS_AGENT is not executable - STEMS unavailable"
else
    mods_log "stemd_client missing at $MODS_AGENT - STEMS unavailable"
fi

# 3) SSH, only with a key installed by usb_update.sh. No key, no port 22.
#    --no-block: this runs inside a unit systemd is still starting. `start`
#    not `enable`: /etc is ro.
if [ -f "$MODS_SSH_KEYS" ]; then
    mods_log "$MODS_SSH_KEYS present - starting SSH"
    systemctl --no-block start dropbear    2>/dev/null || true
    systemctl --no-block start sshd.socket 2>/dev/null || true
fi

# Keep the last boot's log where a power cycle cannot erase it. Best-effort.
# Skipped after a removal: going stock leaves nothing behind.
if mkdir -p "$MODS_LOG_DIR" 2>/dev/null; then
    cp "$MODS_LOG" "$MODS_LOG_KEEP" 2>/dev/null || true
fi

fi

# ------- fall through into the deck's own apl_start.sh (appended below) -------
