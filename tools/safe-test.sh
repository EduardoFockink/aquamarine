#!/usr/bin/bash
# Dead-man switch for trying a new aquamarine build: unless the next login is confirmed in time, it reinstalls
# the previous package and restarts the display manager (with autologin, the session comes back on the
# package that worked). Written for SDDM autologin into Hyprland; override DM / COMPOSITOR otherwise. The
# desktop user defaults to the owner of this script (USER_NAME to override).
#
# 1. Install the build under test (sudo pacman -U new.pkg.tar.zst).
# 2. Arm this as root in a unit that survives the logout, passing the package to fall back to:
#      sudo systemd-run --unit=aq-safe-test /path/to/tools/safe-test.sh /path/to/previous.pkg.tar.zst
# 3. Log out and back in. Within WINDOW seconds, click "Keep" in the dialog that opens, or run:
#      /path/to/tools/safe-test.sh ok
#    The dialog is a zenity window, not a notification: notification daemons drop it under "do not disturb"
#    (2026-11-13: a working build was rolled back because the prompt never showed). A notification is sent too.
#
#    A reboot instead of a logout is fine: the armed state is kept on disk and a boot-time unit
#    (aq-safe-test-resume.service) picks it up, asks, and rolls back the same way. Until 2026-11-13 the switch
#    lived only in the transient unit, and a reboot silently disarmed it.
#
# Follow it with `journalctl -u aq-safe-test -u aq-safe-test-resume -f`; disarm with
# `sudo systemctl stop aq-safe-test` (or `sudo /path/to/tools/safe-test.sh disarm` after a reboot).
# Rehearsal without touching anything: DRY=1 only prints what it would do; TEST=1 skips waiting for the
# logout and treats the running compositor as the new one. Timeouts can be overridden from the environment.

USER_NAME=${USER_NAME:-$(stat -c %U "$(readlink -f "$0")")}
USER_UID=$(id -u "$USER_NAME")
DM=${DM:-sddm}
COMPOSITOR=${COMPOSITOR:-Hyprland}
LOGOUT_WAIT=${LOGOUT_WAIT:-900} # s for the logout to happen
LOGIN_WAIT=${LOGIN_WAIT:-180}   # s, after the logout, for a new compositor to start
WINDOW=${WINDOW:-120}           # s to confirm once the new compositor is up
OK=/run/user/$USER_UID/aq-safe-test-ok
SELF=$(readlink -f "$0")
STATE_DIR=${STATE_DIR:-/var/lib/aq-safe-test} # survives a reboot
STATE=$STATE_DIR/armed
RESUME_NAME=aq-safe-test-resume.service
RESUME_UNIT=/etc/systemd/system/$RESUME_NAME

log() { echo "safe-test: $*"; }

# Armed across a reboot: the package to fall back to, and a unit that runs `safe-test.sh resume` at boot
persist() {
    if [ -n "$DRY" ]; then
        log "(dry run) would write $STATE and enable $RESUME_NAME"
        return
    fi
    mkdir -p "$STATE_DIR" && printf '%s\n' "$ROLLBACK" > "$STATE"
    cat > "$RESUME_UNIT" << EOF
[Unit]
Description=aquamarine safe-test: resume after a reboot (rolls back unless the login is confirmed)
ConditionPathExists=$STATE

[Service]
Type=simple
Environment=DM=$DM COMPOSITOR=$COMPOSITOR USER_NAME=$USER_NAME LOGIN_WAIT=$LOGIN_WAIT WINDOW=$WINDOW
ExecStart=$SELF resume

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable --quiet "$RESUME_NAME" || log "could not enable $RESUME_NAME: a reboot would disarm this"
}
disarm() {
    [ -n "$DRY" ] && return
    rm -f -- "$STATE_DIR/armed"
    if [ -e "/etc/systemd/system/$RESUME_NAME" ]; then
        systemctl disable --quiet "$RESUME_NAME" 2> /dev/null
        rm -f -- "/etc/systemd/system/$RESUME_NAME"
        systemctl daemon-reload
    fi
}

RESUME=""
case "$1" in
    ok)
        touch "$OK" && echo "confirmed: keeping the new package"
        exit 0
        ;;
    disarm)
        disarm && echo "disarmed"
        exit 0
        ;;
    resume)
        [ -f "$STATE" ] || exit 0
        ROLLBACK=$(head -1 "$STATE")
        RESUME=1
        ;;
    *) ROLLBACK=${1:?usage: safe-test.sh <package to fall back to> | ok | disarm} ;;
esac

# compositor processes that existed when this was armed (the running session, plus any stale one)
BEFORE=()
is_before() {
    local p
    for p in "${BEFORE[@]}"; do [ "$p" = "$1" ] && return 0; done
    return 1
}

# A stuck compositor can survive the display manager's restart (seen 2026-11-13: a test session with no picture
# stayed alive and kept the GPUs busy), so the rollback kills every compositor that was not there when armed.
kill_new_compositors() {
    local p sig
    for sig in TERM KILL; do
        for p in $(pgrep -x "$COMPOSITOR"); do
            is_before "$p" && [ -z "$TEST" ] && continue
            log "sending SIG$sig to $COMPOSITOR PID $p"
            kill -"$sig" "$p" 2>/dev/null
        done
        [ "$sig" = TERM ] && sleep 3
    done
}

rollback() {
    log "ROLLING BACK: $1"
    disarm
    if [ -n "$DRY" ]; then
        log "(dry run) pacman -U --noconfirm $ROLLBACK"
        [ "$2" = session ] && log "(dry run) kill the new $COMPOSITOR, systemctl restart $DM"
        exit 3
    fi
    pacman -U --noconfirm "$ROLLBACK" || log "pacman failed (exit $?)"
    if [ "$2" = session ]; then
        # stop the display manager first: if only the compositor is killed, the display manager starts its greeter
        # on its own and is then restarted on top of that (seen 2026-11-13, its X server aborted)
        log "stopping $DM"
        systemctl stop "$DM"
        kill_new_compositors
        log "starting $DM (autologin brings the session back on the previous package)"
        systemctl start "$DM"
    fi
    exit 3
}

[ -n "$DRY" ] || [ "$(id -u)" = 0 ] || { echo "must run as root (see the header)"; exit 1; }
[ -f "$ROLLBACK" ] || { echo "rollback package not found: $ROLLBACK"; disarm; exit 1; }
ROLLBACK=$(readlink -f "$ROLLBACK") # the boot-time unit runs from another directory
if [ -n "$RESUME" ]; then
    log "resumed after a reboot. Rollback package: $ROLLBACK"
else
    log "armed. Rollback package: $ROLLBACK"
    persist
fi

# Stopped by hand = disarm. Stopped because the system is going down = stay armed, the boot-time unit resumes.
on_term() {
    if [ "$(systemctl is-system-running 2> /dev/null)" = stopping ]; then
        log "the system is shutting down: still armed, resuming after the reboot"
    else
        log "stopped: disarmed"
        disarm
    fi
    exit 0
}
trap on_term TERM INT

# 1) wait for the running compositor to exit (logout). It is the NEWEST one: an older one still alive is stale
#    (seen 2026-11-13: watching a stuck test compositor, the real logout went unnoticed and nothing rolled back)
mapfile -t BEFORE < <(pgrep -x "$COMPOSITOR")
OLD=$(pgrep -n -x "$COMPOSITOR")
if [ "${#BEFORE[@]}" -gt 1 ]; then
    log "WARNING: ${#BEFORE[@]} $COMPOSITOR processes are running (${BEFORE[*]}); watching the newest, $OLD."
    log "WARNING: the others are probably stuck sessions; consider rebooting before testing."
fi
if [ -n "$RESUME" ]; then
    BEFORE=() # after a reboot every compositor is the new one
elif [ -z "$TEST" ] && [ -n "$OLD" ]; then
    log "waiting for the logout ($COMPOSITOR PID $OLD, up to ${LOGOUT_WAIT}s)"
    for ((i = 0; i < LOGOUT_WAIT; i++)); do
        kill -0 "$OLD" 2>/dev/null || break
        sleep 1
    done
    # without a logout the current session is fine, but the next login would have no safety net
    kill -0 "$OLD" 2>/dev/null && rollback "no logout within ${LOGOUT_WAIT}s" no-session
    log "old $COMPOSITOR exited"
fi

# 2) wait for a new compositor
NEW=""
for ((i = 0; i < LOGIN_WAIT; i++)); do
    if [ -n "$TEST" ]; then
        NEW=$OLD
    else
        for p in $(pgrep -x "$COMPOSITOR"); do is_before "$p" || NEW=$p; done
    fi
    [ -n "$NEW" ] && break
    sleep 1
done
[ -n "$NEW" ] || rollback "no new $COMPOSITOR within ${LOGIN_WAIT}s" session
log "new $COMPOSITOR: PID $NEW. ${WINDOW}s to confirm"

# 3) confirmation window: a dialog in the new session, plus a notification
rm -f "$OK"
as_user() { # runs "$@" as the desktop user inside the newest Wayland session
    local sock
    sock=$(command ls -t /run/user/$USER_UID/ 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
    runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR=/run/user/$USER_UID WAYLAND_DISPLAY="$sock" GDK_BACKEND=wayland \
        DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$USER_UID/bus "$@"
}
(
    [ -n "$DRY" ] && exit 0
    command -v zenity > /dev/null || { log "zenity is not installed: no dialog, only the notification"; exit 0; }
    sleep 3
    END=$((SECONDS + WINDOW - 3))
    while [ $SECONDS -lt $END ] && [ ! -e "$OK" ]; do
        T0=$SECONDS
        as_user zenity --question --title="aquamarine safe-test" --width=460 --ok-label="Keep" --cancel-label="Not now" \
            --timeout=$((END - SECONDS)) \
            --text="<b>Keep the new aquamarine?</b>\n\nIf the picture is fine, click <b>Keep</b>.\nWithout an answer the previous package is reinstalled and the session restarts (${WINDOW} s after login)." \
            > /dev/null 2>&1
        RC=$?
        if [ $RC = 0 ]; then
            touch "$OK"
            exit 0
        fi
        # an exit within 2 s is the dialog failing to open (the session is still coming up): try again.
        # Later, it is "Not now" or the timeout: stop asking.
        [ $((SECONDS - T0)) -ge 2 ] && exit 0
        sleep 2
    done
) &
(
    [ -n "$DRY" ] && exit 0
    sleep 8
    R=$(timeout "$WINDOW" runuser -u "$USER_NAME" -- env DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$USER_UID/bus \
        notify-send -u critical -a aquamarine -A ok=Keep -w \
        "Keep the new aquamarine?" "Click Keep within ${WINDOW}s, or it rolls back and restarts the session.")
    [ "$R" = ok ] && touch "$OK"
) &

for ((i = 0; i < WINDOW; i++)); do
    if [ -e "$OK" ]; then
        log "confirmed after ${i}s: keeping the new package"
        rm -f "$OK"
        disarm
        exit 0
    fi
    sleep 1
done
rollback "not confirmed within ${WINDOW}s" session
