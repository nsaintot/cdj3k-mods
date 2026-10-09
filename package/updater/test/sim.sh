#!/bin/bash
# sim.sh - dry-run the install / runtime / removal scripts against a fake deck,
# inside a container, so their behaviour can be checked before touching real
# hardware. Run it via test/dryrun.sh (which provides the container).
#
# It models what makes the mechanism safe: pdj.tar.gz is an overlay upper layer
# over /home/root, whose lower layer is the read-only stock rootfs. Each boot
# the merged view is rebuilt as (stock + archive, archive wins), and whatever
# /home/root/scripts/apl_start.sh resolves to is executed, as EP122.service
# would. The deck's hardware touch points (block devices, systemctl,
# gui_image, ...) are stubbed; the update scripts run unmodified, straight from
# updater/src/.
#
# The main claim under test: the phase `rm`s edit the extracted archive in a
# mktemp dir, never the live system, so the stock rootfs is byte-identical from
# first boot to last. That is asserted after every step.
set -u

SRC=/pkg/updater/src
ROOT=/sim
BIN=$ROOT/bin
STOCK=$ROOT/stock          # the read-only lower layer (stock rootfs slice)
SETTINGS=$ROOT/settings    # the settings partition (mmcblk1p7), a separate mount
ISO=$ROOT/iso              # what the .UPD would mount (scripts + dummy binaries)

FAILED=0
pass() { printf '  \033[32mPASS\033[0m  %s\n' "$*"; }
fail() { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; FAILED=1; }
step() { printf '\n== %s ==\n' "$*"; }

# --- the integrity metric -----------------------------------------------------
# The metric must cover a live tree the scripts can reach (they only touch
# /home/root, /mnt, /media, /tmp and mktemp dirs); a checksum of $STOCK
# (/sim/stock) could never fail. md5sum over `-type f` misses modes, ownership,
# symlinks, deleted empty dirs and device nodes, all of which a bad update can
# damage; `tar` captures every one of them in one hash.
# PROTECTED: the live system outside the overlay. Nothing this package does may
# touch these, in any phase.
PROTECTED_PATHS="/etc /usr"
protected_sum() {
    tar --numeric-owner --sort=name -cf - $PROTECTED_PATHS 2>/dev/null | md5sum
}

# A canary: if a deliberate chmod does not move the hash, the metric is blind
# and every assertion built on it is meaningless, so the run fails.
protected_selftest() {
    local before after probe=/etc/.canary
    : > "$probe"; chmod 600 "$probe"; before=$(protected_sum)
    chmod 755 "$probe";               after=$(protected_sum)
    rm -f "$probe"
    [ "$before" != "$after" ]
}

# A script may change what the deck sees in /home/root only by rewriting the
# overlay archive, never by writing to the live tree. The snapshot is taken at
# boot, right after the overlay is applied and before the launcher runs; a
# phase that rm -rf's or chmods the live /home/root diverges from it even when
# the archive is correct. The archive itself is not a valid reference: a phase
# rewrites it mid-boot, so the live tree lags it until the next boot.
EXPECT=/sim/expect
live_snapshot() { rm -rf "$EXPECT"; mkdir -p "$EXPECT"; cp -a /home/root/. "$EXPECT/" 2>/dev/null; }
live_untouched() { diff -r --no-dereference "$EXPECT" /home/root >/dev/null 2>&1; }

# Extract the current overlay archive for inspection. Fails loudly, so "X was
# removed from the archive" assertions cannot pass against a corrupt or absent
# archive.
peek() {
    local t; t=$(mktemp -d)
    if ! tar xzf /mnt/pdj.tar.gz -C "$t" 2>/dev/null; then
        echo "__PEEK_FAILED__"; return 1
    fi
    echo "$t"
}

# ---------------------------------------------------------------- fake deck ----
setup() {
    VARIANT=${1:-rockchip}
    rm -rf "$ROOT"; mkdir -p "$BIN" "$STOCK/scripts" "$STOCK/pdj" "$ISO/mods"

    # Stock lower layer: a minimal launcher that ends the way the real one does
    # (cd /home/root/pdj; ./EP<n>), and a dummy "app" that reports LD_PRELOAD so
    # we can prove the shim would be inherited.
    cat > "$STOCK/scripts/apl_start.sh" <<'EOF'
#!/bin/bash
# Shaped like the deck's own launcher: helper tools first, then one of three
# binaries chosen by /tmp/testmode.
/home/root/pdj/helper
testmode=off
[ -f /tmp/testmode ] && testmode=$(cat /tmp/testmode)
if [ "$testmode" = "off" ] ; then
    if [ -x /home/root/pdj/EP1000 ] ; then
        cd /home/root/pdj
        ./EP1000
    fi
elif [ "$testmode" = "on1" ] ; then
    if [ -x /home/root/pdj/EP1000TestMode ] ; then
        cd /home/root/pdj
        ./EP1000TestMode
    fi
fi
EOF
    cat > "$STOCK/pdj/EP1000" <<'EOF'
#!/bin/sh
echo "[EP1000] app up; LD_PRELOAD=${LD_PRELOAD:-<unset>}"
EOF
    cat > "$STOCK/pdj/EP1000TestMode" <<'EOF'
#!/bin/sh
echo "[EP1000TestMode] test mode up; LD_PRELOAD=${LD_PRELOAD:-<unset>}"
EOF
    cat > "$STOCK/pdj/helper" <<'EOF'
#!/bin/sh
echo "[helper] a launcher tool ran; LD_PRELOAD=${LD_PRELOAD:-<unset>}"
EOF
    chmod +x "$STOCK/scripts/apl_start.sh" "$STOCK/pdj/EP1000" \
             "$STOCK/pdj/EP1000TestMode" "$STOCK/pdj/helper"

    # The settings partition (mmcblk1p7 on the deck), a separate mount from the
    # overlay. The deck's own files sit beside ours and must survive a removal.
    # A symlink in the stock tree, because it is a mount point on the deck and
    # its contents are not part of the overlay.
    rm -rf "$SETTINGS"; mkdir -p "$SETTINGS"
    printf 'MODS\1\0' > "$SETTINGS/CDJ3K_MODSETTINGS.DAT"
    cp "$SETTINGS/CDJ3K_MODSETTINGS.DAT" "$SETTINGS/CDJ3K_MODSETTINGS.DAT.BAK"
    printf 'DECK-OWN-DJ-SETTING'   > "$SETTINGS/CDJ3K_DJSETTING.DAT"
    printf 'DECK-OWN-DJ-SETTING'   > "$SETTINGS/CDJ3K_DJSETTING.DAT.BAK"
    printf 'DECK-OWN-MY-SETTING'   > "$SETTINGS/CDJ3K_MYSETTING.DAT"
    ln -sfn "$SETTINGS" "$STOCK/settings"

    # The ISO the deck would mount: the real update scripts + dummy binaries
    # (this harness tests the script plumbing, not the aarch64 binaries).
    cp "$SRC/usb_update.sh" "$SRC/phase1.sh" "$SRC/phase2.sh" \
       "$SRC/payload.sh"    "$SRC/uninstall.sh" "$ISO/"
    printf '\177ELF (dummy shim placeholder)\n' > "$ISO/mods/ep122_shim.so"
    cat > "$ISO/mods/stemd_client" <<'EOF'
#!/bin/sh
echo "[stemd_client stub] up; LD_PRELOAD=${LD_PRELOAD:-<unset>}"
EOF
    chmod +x "$ISO/mods/stemd_client"

    # A stock deck's overlay is not empty: the stock update ships pdj.tar.gz
    # containing cache.conf, so every install starts from that.
    rm -rf /sim/stockoverlay; mkdir -p /sim/stockoverlay
    : > /sim/stockoverlay/cache.conf
    tar -czf /mnt/pdj.tar.gz -C /sim/stockoverlay . 2>/dev/null || true

    # Overlay partition (persists across boots) and the update media, the latter
    # a real tmpfs mount so the scripts find CDJ3Kv000.UPD via /proc/mounts.
    mkdir -p /mnt /media
    mountpoint -q /media || busybox mount -t tmpfs none /media
    : > /media/CDJ3Kv000.UPD

    # Exactly one layout must be present, so the scripts' if/elif picks the
    # branch under test and the other is unreachable.
    if [ "$VARIANT" = renesas ]; then
        rm -f /dev/mmcblk1p8 /dev/mmcblk1p7
        [ -b /dev/mmcblk0p5 ] || mknod /dev/mmcblk0p5 b 179 5
    else
        rm -f /dev/mmcblk0p5
        [ -b /dev/mmcblk1p8 ] || mknod /dev/mmcblk1p8 b 179 8
        # The settings partition is its own device, mounted only by the removal.
        [ -b /dev/mmcblk1p7 ] || mknod /dev/mmcblk1p7 b 179 7
    fi

    # The Renesas tail writes to /sys/class/vtconsole/vtcon1/bind, and under
    # `set -e` a failed redirect kills the phase. /sys is read-only even in a
    # privileged container, so shadow just that subtree with a tmpfs.
    if [ ! -e /sys/class/vtconsole/vtcon1/bind ]; then
        mountpoint -q /sys/class || mount -t tmpfs none /sys/class 2>/dev/null || true
        mkdir -p /sys/class/vtconsole/vtcon1 2>/dev/null || true
        : > /sys/class/vtconsole/vtcon1/bind 2>/dev/null || true
    fi
    # Fake GPIO tree so the front-panel LED path really runs. Real sysfs creates
    # gpioN/ when you write to export; here it is pre-made.
    mkdir -p /sys/class/gpio/gpio71 2>/dev/null || true
    : > /sys/class/gpio/export 2>/dev/null || true
    : > /sys/class/gpio/unexport 2>/dev/null || true
    : > /sys/class/gpio/gpio71/direction 2>/dev/null || true
    : > /sys/class/gpio/gpio71/value 2>/dev/null || true

    # ---- stubs for the deck's hardware/service commands ----
    # mount: `remount` is a no-op; mounting the overlay partition to a mktemp
    # dir is modelled by pointing that dir at /mnt (our overlay partition).
    cat > "$BIN/mount" <<'EOF'
#!/bin/sh
opts=; rest=
while [ $# -gt 0 ]; do case "$1" in -o) opts=$2; shift 2;; *) rest="$rest $1"; shift;; esac; done
set -- $rest; dev=$1; tgt=$2
echo "$opts|$dev|$tgt" >> /sim/mount.log
case "$opts" in *remount*) exit 0;; esac
# Only the overlay partition may be mounted for writing. Anything else is a bug
# that would destroy a real partition, so refuse it.
case "$dev" in
  /dev/mmcblk1p8|/dev/mmcblk0p5) rmdir "$tgt" 2>/dev/null; ln -sfn /mnt "$tgt"; exit 0;;
  /dev/mmcblk1p7)                rmdir "$tgt" 2>/dev/null; ln -sfn /sim/settings "$tgt"; exit 0;;
  *) echo "REFUSED: attempt to mount unexpected device: $dev" >&2; exit 32;;
esac
EOF
    cat > "$BIN/umount" <<'EOF'
#!/bin/sh
for a in "$@"; do [ -L "$a" ] && rm -f "$a"; done; exit 0
EOF
    # systemctl: log what was asked; `reboot` just returns so the caller's
    # `exit 0` ends the boot (our loop performs the next boot).
    cat > "$BIN/systemctl" <<'EOF'
#!/bin/sh
echo "[stub systemctl] $*"
echo "$*" >> /sim/systemctl.log
exit 0
EOF
    # pidof: nothing is ever already running, so payload always starts the agent.
    printf '#!/bin/sh\nexit 1\n' > "$BIN/pidof"
    cat > "$BIN/gui_image" <<'EOF'
#!/bin/sh
printf '%s|%s\n' "$#" "$*" >> /sim/gui_image.log
echo "[stub gui_image] $*"
exit 0
EOF
    # The Renesas tail ends in `sleep 30d` waiting for a human to pull the power.
    # Return immediately from those so the suite models the power cycle instead
    # of hanging for a month; ordinary short sleeps still really sleep.
    cat > "$BIN/sleep" <<'EOF'
#!/bin/sh
case "$1" in *d|*h|*m) exit 0;; esac
exec /bin/sleep "$@"
EOF
    # fw_printenv: the deck's u-boot environment. `release` is what the installer
    # gates on; a test writes /sim/fw_release to change it.
    [ -f /sim/fw_release ] || echo "3.22" > /sim/fw_release
    cat > "$BIN/fw_printenv" <<'EOF'
#!/bin/sh
[ "$1" = "-n" ] && shift
case "$1" in
  release) cat /sim/fw_release ;;
  model)   echo "CDJ-3000" ;;
  *)       exit 1 ;;
esac
EOF
    chmod +x "$BIN/fw_printenv"

    for c in pkill openvt killall; do
        printf '#!/bin/sh\necho "[stub %s] $*"\nexit 0\n' "$c" > "$BIN/$c"
    done
    chmod +x "$BIN"/*
    export PATH="$BIN:$PATH"
}

# One boot: rebuild the merged /home/root = stock (+) archive, then run whatever
# apl_start.sh resolves to, exactly as the service would.
BOOT_RC=0
BOOT_DAMAGED=0
boot() {
    rm -rf /home/root; mkdir -p /home/root
    cp -a "$STOCK/." /home/root/
    [ -f /mnt/pdj.tar.gz ] && tar xzf /mnt/pdj.tar.gz -C /home/root 2>/dev/null
    # exec, not `bash <file>`: that is what the service does, so a missing exec
    # bit or a broken shebang has to fail here too.
    live_snapshot
    /home/root/scripts/apl_start.sh; BOOT_RC=$?
    # Checked now, before the next boot's reset would heal anything a script
    # wrote directly to the live tree.
    live_untouched || BOOT_DAMAGED=1
    return $BOOT_RC
}

# ---------------------------------------------------------------- the run ------
setup
if protected_selftest; then
    echo "metric self-test: ok (a chmod moves the hash - the check CAN fail)"
else
    echo "metric self-test: BROKEN - the integrity check cannot detect changes"; FAILED=1
fi
# After the self-test: creating and removing the canary moves /etc's own mtime,
# which the tar-based metric (correctly) notices.
BASELINE=$(protected_sum)
echo "protected-tree baseline: $BASELINE  (paths: $PROTECTED_PATHS)"

step "STEP 1  install: usb_update.sh stages the overlay"
bash "$ISO/usb_update.sh" "$ISO" en; rc=$?
[ $rc = 0 ] && pass "usb_update exited 0" || fail "usb_update exited $rc"
P=$(peek)
[ -f "$P/scripts/apl_start.sh" ] && grep -q 'APP_ORIGINAL=' "$P/scripts/apl_start.sh" \
    && pass "archive apl_start = phase1" || fail "archive apl_start is not phase1"
[ -f "$P/phase2.sh" ]  && pass "archive has phase2.sh"  || fail "no phase2.sh in archive"
[ -f "$P/payload.sh" ] && pass "archive has payload.sh" || fail "no payload.sh in archive"
[ -f "$P/mods/ep122_shim.so" ] && [ -f "$P/mods/stemd_client" ] \
    && pass "archive has mods/{ep122_shim.so,stemd_client}" || fail "mods missing from archive"
grep -q '^71$' /sys/class/gpio/export && pass "front-panel LED GPIO 71 exported" || fail "LED not exported"
grep -q '^out$' /sys/class/gpio/gpio71/direction && pass "LED GPIO set to output" || fail "LED direction not set"
grep -qE '^[01]$' /sys/class/gpio/gpio71/value && pass "LED is being driven (blink running)" || fail "LED never driven"
# gui_image never returns: it paints a full screen and stays up until killed,
# so the deck's recovery always backgrounds it. A foreground call hangs the
# install (the log stops mid-line and the deck falls back to asking for the
# stick). Check the source, since the stub cannot block.
bad=$(grep -n 'gui_image' "$SRC"/usb_update.sh \
      | grep -vE '^[0-9]+:[[:space:]]*#' \
      | grep -vE '&[[:space:]]*$' \
      | grep -v 'pkill' \
      | grep -v 'D007 "$LANGUAGE" ""' || true)
[ -z "$bad" ] && pass "no blocking (foreground) gui_image call in the installer" \
    || fail "foreground gui_image would hang the install: $bad"
[ "$(protected_sum)" = "$BASELINE" ] && pass "/etc /usr untouched" || fail "PROTECTED TREE CHANGED!"
[ "$BOOT_DAMAGED" = 0 ] && pass "live /home/root matches (stock + overlay) exactly" || fail "a script wrote directly to the live tree!"

step "STEP 2  boot -> phase1 (runs as the launcher)"
boot >/dev/null 2>&1; rc=$?
[ $rc = 0 ] && pass "phase1 boot exited 0" || fail "phase1 boot exited $rc"
P=$(peek)
[ ! -e "$P/scripts/apl_start.sh" ] && pass "phase1 dropped apl_start FROM THE ARCHIVE (not the deck)" \
    || fail "apl_start still in archive"
[ -f "$P/pdj/EP1000" ] && grep -q 'STOCK_LAUNCHER=' "$P/pdj/EP1000" \
    && pass "phase1 staged phase2 as pdj/EP1000 (shadow of the app)" || fail "phase2 shadow missing"
[ -f "$P/mods/ep122_shim.so" ] && [ -f "$P/mods/stemd_client" ] \
    && pass "mods/ carried through phase1 untouched" || fail "mods lost in phase1"
[ "$(protected_sum)" = "$BASELINE" ] && pass "/etc /usr still untouched" || fail "PROTECTED TREE CHANGED!"
[ "$BOOT_DAMAGED" = 0 ] && pass "live /home/root still matches (stock + overlay)" || fail "a script wrote directly to the live tree!"

step "STEP 3  boot -> stock apl_start launches phase2 (the shadowed app)"
boot >/dev/null 2>&1; rc=$?
[ $rc = 0 ] && pass "phase2 boot exited 0" || fail "phase2 boot exited $rc"
P=$(peek)
[ -f "$P/scripts/apl_start.sh" ] && pass "phase2 installed the runtime apl_start" || fail "no apl_start after phase2"
grep -q 'MODS_SHIM=' "$P/scripts/apl_start.sh" && pass "runtime apl_start begins with payload" || fail "payload prefix missing"
pay_ln=$(grep -n 'MODS_SHIM=' "$P/scripts/apl_start.sh" | head -1 | cut -d: -f1)
stk_ln=$(grep -n 'testmode=off'         "$P/scripts/apl_start.sh" | head -1 | cut -d: -f1)
[ -n "$stk_ln" ] && pass "stock launcher appended after payload" || fail "stock launcher not appended"
[ -n "$pay_ln" ] && [ -n "$stk_ln" ] && [ "$pay_ln" -lt "$stk_ln" ] \
    && pass "payload comes BEFORE the stock launcher (order, not just presence)" \
    || fail "payload/stock order is wrong"
[ ! -e "$P/pdj" ] && pass "phase2 dropped the pdj/ shadow FROM THE ARCHIVE" || fail "pdj/ shadow still in archive"
[ -f "$P/mods/ep122_shim.so" ] && [ -f "$P/mods/stemd_client" ] \
    && pass "mods/ carried through phase2 untouched" || fail "mods lost in phase2"
[ "$(protected_sum)" = "$BASELINE" ] && pass "/etc /usr still untouched" || fail "PROTECTED TREE CHANGED!"
[ "$BOOT_DAMAGED" = 0 ] && pass "live /home/root still matches (stock + overlay)" || fail "a script wrote directly to the live tree!"

step "STEP 4  boot -> installed runtime (payload, then the real app)"
: > /tmp/cdj3k-mods.log; : > /tmp/stemd_client.log
out=$(boot 2>/dev/null); sleep 0.3
grep -q 'preloaded on the application line' /tmp/cdj3k-mods.log \
    && pass "payload reports the launcher-line preload" || fail "payload did not report the preload"
grep -q 'stemd_client stub] up' /tmp/stemd_client.log \
    && pass "payload started the stemd_client sidecar" || fail "sidecar not started"
echo "$out" | grep -q 'EP1000] app up; LD_PRELOAD=/home/root/mods/ep122_shim.so' \
    && pass "the app inherited LD_PRELOAD (shim would load)" || fail "app did not inherit LD_PRELOAD"
grep -qi 'starting SSH' /tmp/cdj3k-mods.log \
    && fail "SSH started with no key present!" || pass "SSH stayed off (no authorized_keys)"

step "STEP 4b  the preload is scoped to the application, nothing else"
P=$(peek)
grep -q '^[[:space:]]*LD_PRELOAD=/home/root/mods/ep122_shim.so \./EP1000$' "$P/scripts/apl_start.sh" \
    && pass "launcher carries LD_PRELOAD on the app line itself" || fail "app line not rewritten"
grep -q 'LD_PRELOAD=.*EP1000TestMode' "$P/scripts/apl_start.sh" \
    && fail "TestMode line ALSO got the shim" || pass "TestMode line left stock"
grep -q '^export LD_PRELOAD' "$P/scripts/apl_start.sh" \
    && fail "payload still exports a global LD_PRELOAD" || pass "no global export in the launcher"
out=$(boot 2>/dev/null)
echo "$out" | grep -q 'helper] a launcher tool ran; LD_PRELOAD=<unset>' \
    && pass "helper tools run WITHOUT the shim" || fail "a helper inherited the shim"
echo "$out" | grep -q 'EP1000] app up; LD_PRELOAD=/home/root/mods/ep122_shim.so' \
    && pass "the application still gets the shim" || fail "app did not get the shim"

echo "on1" > /tmp/testmode        # <- front panel boots into service mode
out=$(boot 2>/dev/null)
echo "$out" | grep -q 'TestMode] test mode up; LD_PRELOAD=<unset>' \
    && pass "TEST MODE runs completely stock (no shim)" || fail "TestMode inherited the shim!"
# Service mode is the front-panel removal: the overlay goes, this boot carries on.
[ ! -f /mnt/pdj.tar.gz ] \
    && pass "service mode removed the overlay" || fail "service mode left pdj.tar.gz behind"
[ ! -d /mnt/cdj3k-mods-logs ] \
    && pass "service mode cleared the logs too" || fail "service mode left the log dir"
[ ! -e /sim/settings/CDJ3K_MODSETTINGS.DAT ] && [ ! -e /sim/settings/CDJ3K_MODSETTINGS.DAT.BAK ] \
    && pass "service mode removed MOD SETTINGS and its .BAK" \
    || fail "service mode left MOD SETTINGS behind"
[ -s /sim/settings/CDJ3K_DJSETTING.DAT ] && [ -s /sim/settings/CDJ3K_DJSETTING.DAT.BAK ] \
    && [ -s /sim/settings/CDJ3K_MYSETTING.DAT ] \
    && pass "the deck's OWN settings files were not touched" \
    || fail "a stock CDJ3K_*.DAT was destroyed"
rm -f /tmp/testmode
out=$(boot 2>/dev/null)
echo "$out" | grep -q 'LD_PRELOAD=/home/root/mods/ep122_shim.so' \
    && fail "deck still modded after a service-mode removal" \
    || pass "next normal boot is stock"
[ "$BOOT_DAMAGED" = 0 ] \
    && pass "removal touched nothing in the live tree" || fail "removal wrote to the live tree"

step "STEP 5  reinstall WITH authorized_keys -> SSH comes up"
rm -f /mnt/pdj.tar.gz
echo 'ssh-ed25519 AAAADUMMYKEY sim@test' > /media/authorized_keys
bash "$ISO/usb_update.sh" "$ISO" en >/dev/null 2>&1
boot >/dev/null 2>&1; boot >/dev/null 2>&1     # advance through phase1, phase2
P=$(peek)
[ -f "$P/.ssh/authorized_keys" ] && pass "authorized_keys carried into the overlay" || fail "authorized_keys not installed"
m=$(stat -c '%a' "$P/.ssh/authorized_keys" 2>/dev/null)
[ "$m" = 600 ] && pass "authorized_keys mode is 600" || fail "authorized_keys mode is $m (want 600)"
: > /tmp/cdj3k-mods.log; : > /sim/systemctl.log
boot >/dev/null 2>&1
grep -q 'present - starting SSH' /tmp/cdj3k-mods.log && pass "payload started SSH (key present)" || fail "SSH not started despite key"
grep -q 'start dropbear' /sim/systemctl.log && pass "dropbear start was invoked" || fail "dropbear not invoked"

step "STEP 5b  logs: named explicitly, and written where they can be read"
[ -f /media/install.log ] && pass "install.log on the stick" || fail "no install.log on the stick"
[ -f /media/phase1.log ]  && pass "phase1.log on the stick (not apl_start.sh.log)" || fail "no phase1.log"
[ -f /media/phase2.log ]  && pass "phase2.log on the stick (not EP1000.log)" || fail "no phase2.log"
[ -e /media/apl_start.sh.log ] || [ -e /media/EP1000.log ] \
    && fail "a phase still logged under a stock-looking name" || pass "no stock-looking log names left"
[ -f /mnt/cdj3k-mods-logs/runtime.log ] && pass "runtime log kept on the overlay (survives reboot)" \
    || fail "runtime log not persisted"
grep -qE 'shim (present|missing)' /mnt/cdj3k-mods-logs/runtime.log 2>/dev/null \
    && pass "persisted runtime log has the boot's mod status" || fail "persisted runtime log has no status"

step "STEP 5c  STICK PULLED before a phase -> still logged, on the overlay"
rm -f /mnt/pdj.tar.gz /media/*.log
bash "$ISO/usb_update.sh" "$ISO" en >/dev/null 2>&1     # stage a fresh install
rm -rf /mnt/cdj3k-mods-logs
busybox umount /media 2>/dev/null                        # <- the DJ pulls the stick
boot >/dev/null 2>&1                                     # phase1 runs with no media
[ -f /mnt/cdj3k-mods-logs/phase1.log ] \
    && pass "phase1 fell back to the overlay partition" || fail "phase1 logged nowhere (stick absent)"
grep -q 'phase1:' /mnt/cdj3k-mods-logs/phase1.log 2>/dev/null \
    && pass "fallback log carries the phase1 command trace" || fail "fallback log has no trace"
busybox mount -t tmpfs none /media; : > /media/CDJ3Kv000.UPD   # stick back in
boot >/dev/null 2>&1                                          # phase2 -> back on the stick
[ -f /media/phase2.log ] && pass "phase2 logged to the stick again once re-inserted" \
    || fail "phase2 did not resume logging to the stick"
[ "$(protected_sum)" = "$BASELINE" ] && pass "/etc /usr still untouched" || fail "PROTECTED TREE CHANGED!"
[ "$BOOT_DAMAGED" = 0 ] && pass "live /home/root still matches (stock + overlay)" || fail "a script wrote directly to the live tree!"

step "STEP 5d  the installer only ever mounted the overlay partition"
grep -q 'mmcblk1p8' /sim/mount.log && pass "overlay partition /dev/mmcblk1p8 was mounted" || fail "overlay partition never mounted"
bad=$(grep -v 'remount' /sim/mount.log | cut -d'|' -f2 | grep -vE '^/dev/mmcblk(1p8|0p5|1p7)$' | sort -u)
[ -z "$bad" ] && pass "only the overlay and settings partitions were mounted rw" || fail "mounted rw: $bad"

step "STEP 5e  firmware below 3.13 is refused before anything is written"
# 3.12, one step below the floor, is what pins where the boundary sits; a
# major-version case such as 2.99 would also pass a 3.00 floor.
rm -f /mnt/pdj.tar.gz; : > /sim/gui_image.log
echo "3.12" > /sim/fw_release
# mods_fail holds the error image up forever, so the run has to be timed out.
timeout 10 bash "$ISO/usb_update.sh" "$ISO" en >/dev/null 2>&1; rc=$?
[ "$rc" = 124 ] && pass "the installer stopped and held the screen (did not exit)" \
    || fail "expected a hang on the error image, got rc=$rc"
[ ! -f /mnt/pdj.tar.gz ] \
    && pass "nothing was written on a refused firmware" || fail "an overlay was staged on 3.12"
grep -q 'D003' /sim/gui_image.log \
    && pass "the error image (D003) was shown" || fail "no D003 on a refused firmware"

# An older major, which the minor comparison must not let through.
rm -f /mnt/pdj.tar.gz; echo "2.99" > /sim/fw_release
timeout 10 bash "$ISO/usb_update.sh" "$ISO" en >/dev/null 2>&1
[ ! -f /mnt/pdj.tar.gz ] \
    && pass "2.99 is refused too" || fail "an overlay was staged on 2.99"

# ... and a supported one still installs.
echo "3.13" > /sim/fw_release
rm -f /mnt/pdj.tar.gz
bash "$ISO/usb_update.sh" "$ISO" en >/dev/null 2>&1
[ -f /mnt/pdj.tar.gz ] && pass "3.13 installs (the floor is inclusive)" || fail "3.13 was refused"
# An unreadable version must not block the install: the shim gates it at runtime.
rm -f /mnt/pdj.tar.gz; echo "" > /sim/fw_release
bash "$ISO/usb_update.sh" "$ISO" en >/dev/null 2>&1
[ -f /mnt/pdj.tar.gz ] && pass "an unreadable version still installs" || fail "blocked on an unreadable version"
echo "3.22" > /sim/fw_release

step "STEP 6  uninstall -> overlay cleared -> stock"
# Put MOD SETTINGS back: the service-mode test above already removed it, and an
# absent file would make the assertions below pass without exercising anything.
printf 'MODS\1\0' > "$SETTINGS/CDJ3K_MODSETTINGS.DAT"
cp "$SETTINGS/CDJ3K_MODSETTINGS.DAT" "$SETTINGS/CDJ3K_MODSETTINGS.DAT.BAK"
bash "$ISO/uninstall.sh" "$ISO" en; rc=$?
[ $rc = 0 ] && pass "uninstall exited 0" || fail "uninstall exited $rc"
[ ! -f /mnt/pdj.tar.gz ] && pass "uninstall removed pdj.tar.gz (the whole overlay)" || fail "overlay archive still present"
[ ! -e /mnt/cdj3k-mods-logs ] && pass "uninstall cleared the fallback log dir" || fail "log dir left behind after uninstall"
[ ! -e "$SETTINGS/CDJ3K_MODSETTINGS.DAT" ] && [ ! -e "$SETTINGS/CDJ3K_MODSETTINGS.DAT.BAK" ] \
    && pass "uninstall removed MOD SETTINGS and its .BAK" || fail "uninstall left MOD SETTINGS behind"
[ -s "$SETTINGS/CDJ3K_DJSETTING.DAT" ] && [ -s "$SETTINGS/CDJ3K_DJSETTING.DAT.BAK" ] \
    && [ -s "$SETTINGS/CDJ3K_MYSETTING.DAT" ] \
    && pass "the deck's OWN settings files survived the uninstall" \
    || fail "uninstall destroyed a stock CDJ3K_*.DAT"
[ -f /media/uninstall.log ] && pass "uninstall.log on the stick (not usb_update.sh.log)" || fail "no uninstall.log"
out=$(boot 2>/dev/null)
echo "$out" | grep -q 'helper] a launcher tool ran' && pass "deck boots the stock launcher again" || fail "did not revert to stock launcher"
echo "$out" | grep -q 'app up; LD_PRELOAD=<unset>' && pass "app runs with NO shim (LD_PRELOAD unset)" || fail "shim still present after uninstall"

step "STEP 7  RENESAS layout (mmcblk0p5): the same lifecycle on the other variant"
setup renesas
rm -f /mnt/pdj.tar.gz /media/*.log; rm -rf /mnt/cdj3k-mods-logs
: > /sim/gui_image.log; : > /sim/systemctl.log; : > /sim/mount.log

bash "$ISO/usb_update.sh" "$ISO" en; rc=$?
[ $rc = 0 ] && pass "usb_update exited 0 on Renesas" || fail "usb_update exited $rc on Renesas"
grep -q 'mmcblk0p5' /sim/mount.log \
    && pass "mounted the Renesas overlay partition (mmcblk0p5)" || fail "did not mount mmcblk0p5"
grep -q 'mmcblk1p8' /sim/mount.log \
    && fail "touched the Rockchip partition on a Renesas unit" || pass "never touched mmcblk1p8"
grep -q '^3|D007' /sim/gui_image.log \
    && pass "Renesas 3-arg gui_image D007 issued (\"\" trailing arg)" \
    || fail "gui_image form wrong: $(cat /sim/gui_image.log)"

boot >/dev/null 2>&1; r1=$BOOT_RC     # phase1 - no reboot on this variant
boot >/dev/null 2>&1; r2=$BOOT_RC     # phase2
[ "$r1" = 0 ] && [ "$r2" = 0 ] && pass "both phases exited 0 on Renesas" \
    || fail "phase exit codes on Renesas: phase1=$r1 phase2=$r2"
grep -q 'reboot' /sim/systemctl.log \
    && fail "Renesas auto-rebooted (it must ask for a manual power cycle)" \
    || pass "Renesas asks for a manual power cycle, as designed"

P=$(peek)
grep -q 'MODS_SHIM=' "$P/scripts/apl_start.sh" \
    && pass "runtime launcher installed on Renesas" || fail "runtime launcher missing on Renesas"
[ -f "$P/mods/ep122_shim.so" ] && [ -f "$P/mods/stemd_client" ] \
    && pass "both binaries staged on Renesas" || fail "binaries missing on Renesas"
out=$(boot 2>/dev/null)
echo "$out" | grep -q 'EP1000] app up; LD_PRELOAD=/home/root/mods/ep122_shim.so' \
    && pass "Renesas: the application gets the shim" || fail "Renesas: app did not get the shim"
echo "$out" | grep -q 'helper] a launcher tool ran; LD_PRELOAD=<unset>' \
    && pass "Renesas: helper tools still run without the shim" || fail "Renesas: a helper inherited the shim"
[ "$(protected_sum)" = "$BASELINE" ] && pass "/etc /usr untouched on Renesas too" || fail "PROTECTED TREE CHANGED!"

step "STEP 8  UNRECOGNISED launcher -> installs nothing, deck stays stock"
setup rockchip
rm -f /mnt/pdj.tar.gz /media/*.log; rm -rf /mnt/cdj3k-mods-logs
# A launcher that does start the app, but in a form our pattern does not
# recognise (`exec ./EP1000`, not a bare `./EP1000` line). phase2 still runs,
# so it must refuse instead of guessing.
cat > "$STOCK/scripts/apl_start.sh" <<'EOF'
#!/bin/bash
/home/root/pdj/helper
cd /home/root/pdj
exec ./EP1000
EOF
chmod +x "$STOCK/scripts/apl_start.sh"

bash "$ISO/usb_update.sh" "$ISO" en >/dev/null 2>&1
boot >/dev/null 2>&1     # phase1
boot >/dev/null 2>&1     # phase2 - must refuse here
P=$(peek)
[ ! -e "$P/scripts/apl_start.sh" ] \
    && pass "no launcher override left in the overlay" || fail "phase2 installed a launcher anyway"
[ ! -e "$P/mods" ] && pass "mod binaries removed (nothing half-installed)" || fail "mods left behind"
grep -q 'export LD_PRELOAD' "$P"/* 2>/dev/null \
    && fail "fell back to a global LD_PRELOAD export" || pass "no global preload fallback (all-or-nothing kept)"
out=$(boot 2>/dev/null)
echo "$out" | grep -q 'LD_PRELOAD=<unset>' \
    && pass "deck boots completely stock" || fail "deck did not boot stock"
[ "$(protected_sum)" = "$BASELINE" ] && pass "/etc /usr untouched" || fail "PROTECTED TREE CHANGED!"

step "STEP 9  UPDATE an already-modded deck (v1 -> v2): no compounding"
setup rockchip
# Do not clear /mnt/pdj.tar.gz here: setup() has just written the stock
# overlay (cache.conf), and this step tests installing onto a deck that already
# has one.
rm -f /media/*.log; rm -rf /mnt/cdj3k-mods-logs
printf '\177ELF shim-v1\n' > "$ISO/mods/ep122_shim.so"

# --- install v1, all the way to a working deck
bash "$ISO/usb_update.sh" "$ISO" en >/dev/null 2>&1
boot >/dev/null 2>&1; boot >/dev/null 2>&1      # phase1, phase2
out=$(boot 2>/dev/null)
echo "$out" | grep -q 'LD_PRELOAD=/home/root/mods' && pass "v1 installed and running" || fail "v1 did not install"

# --- now ship v2 over the top, exactly as a user re-flashing a new release
printf '\177ELF shim-v2\n' > "$ISO/mods/ep122_shim.so"
bash "$ISO/usb_update.sh" "$ISO" en >/dev/null 2>&1; rc=$?
[ $rc = 0 ] && pass "update install exited 0 on a modded deck" || fail "update exited $rc"
boot >/dev/null 2>&1; boot >/dev/null 2>&1      # phase1, phase2 again

P=$(peek)
grep -q 'shim-v2' "$P/mods/ep122_shim.so" && pass "binaries replaced with v2" || fail "still running v1 binaries"
n=$(grep -c 'MODS_SHIM=' "$P/scripts/apl_start.sh")
[ "$n" = 1 ] && pass "exactly ONE payload prefix (no stacking)" || fail "payload prefix appears $n times"
n=$(grep -c 'testmode=off' "$P/scripts/apl_start.sh")
[ "$n" = 1 ] && pass "exactly ONE copy of the stock launcher" || fail "stock launcher appears $n times"
n=$(grep -c 'LD_PRELOAD=/home/root/mods/ep122_shim.so' "$P/scripts/apl_start.sh")
[ "$n" = 1 ] && pass "exactly ONE preload on the app line" || fail "preload appears $n times"
# The overlay is defined by the update, not by history: installing onto a modded
# deck must land in the same state as installing onto a stock one.
UPD_LIST=$(cd "$P" && find . | sort | md5sum)
setup rockchip >/dev/null 2>&1
bash "$ISO/usb_update.sh" "$ISO" en >/dev/null 2>&1
boot >/dev/null 2>&1; boot >/dev/null 2>&1
Q=$(peek); FRESH_LIST=$(cd "$Q" && find . | sort | md5sum)
[ "$UPD_LIST" = "$FRESH_LIST" ] \
    && pass "update lands in the SAME state as a fresh install (no history)" \
    || fail "updated deck differs from a freshly installed one"
out=$(boot 2>/dev/null)
echo "$out" | grep -q 'EP1000] app up; LD_PRELOAD=/home/root/mods' && pass "updated deck still boots modded" || fail "updated deck broken"
echo "$out" | grep -q 'helper] a launcher tool ran; LD_PRELOAD=<unset>' && pass "still scoped to the app after update" || fail "preload leaked after update"
[ "$(protected_sum)" = "$BASELINE" ] && pass "/etc /usr untouched by the update" || fail "PROTECTED TREE CHANGED!"

step "FINAL  stock rootfs integrity across the whole lifecycle"
[ "$(protected_sum)" = "$BASELINE" ] && pass "/etc /usr byte-identical to baseline (nothing destructive ran)" \
    || fail "STOCK rootfs was modified somewhere!"

echo
if [ $FAILED = 0 ]; then printf '\033[32mALL CHECKS PASSED\033[0m\n'; else printf '\033[31mSOME CHECKS FAILED\033[0m\n'; exit 1; fi
