#!/usr/bin/bash
# Dead-man switch for trying a new aquamarine build: unless the next login is confirmed in time, it reinstalls
# the previous package and restarts the display manager (with autologin, the session comes back on the
# package that worked). Written for SDDM autologin into Hyprland; adjust DM, USER_NAME and USER_UID otherwise.
#
# 1. Install the build under test (sudo pacman -U new.pkg.tar.zst).
# 2. Arm this as root in a unit that survives the logout, passing the package to fall back to:
#      sudo systemd-run --unit=aq-safe-test /path/to/tools/safe-test.sh /path/to/previous.pkg.tar.zst
# 3. Log out and back in. Within WINDOW seconds, click "Keep" on the notification, or run:
#      /path/to/tools/safe-test.sh ok
#
# Follow it with `journalctl -u aq-safe-test -f`; disarm with `sudo systemctl stop aq-safe-test`.
# Rehearsal without touching anything: DRY=1 only prints what it would do; TEST=1 skips waiting for the
# logout and treats the running compositor as the new one. Timeouts can be overridden from the environment.

USER_NAME=${USER_NAME:-eduardo}
USER_UID=${USER_UID:-1000}
DM=${DM:-sddm}
COMPOSITOR=${COMPOSITOR:-Hyprland}
LOGOUT_WAIT=${LOGOUT_WAIT:-900} # s for the logout to happen
LOGIN_WAIT=${LOGIN_WAIT:-180}   # s, after the logout, for a new compositor to start
WINDOW=${WINDOW:-90}            # s to confirm once the new compositor is up
OK=/run/user/$USER_UID/aq-safe-test-ok

if [ "$1" = ok ]; then
    touch "$OK" && echo "confirmed: keeping the new package"
    exit 0
fi

ROLLBACK=${1:?usage: safe-test.sh <package to fall back to> | ok}

log() { echo "safe-test: $*"; }

rollback() {
    log "ROLLING BACK: $1"
    if [ -n "$DRY" ]; then
        log "(dry run) pacman -U --noconfirm $ROLLBACK"
        [ "$2" = session ] && log "(dry run) systemctl restart $DM"
        exit 3
    fi
    pacman -U --noconfirm "$ROLLBACK" || log "pacman failed (exit $?)"
    if [ "$2" = session ]; then
        log "restarting $DM (autologin brings the session back on the previous package)"
        systemctl restart "$DM"
    fi
    exit 3
}

[ -n "$DRY" ] || [ "$(id -u)" = 0 ] || { echo "must run as root (see the header)"; exit 1; }
[ -f "$ROLLBACK" ] || { echo "rollback package not found: $ROLLBACK"; exit 1; }
log "armed. Rollback package: $ROLLBACK"

# 1) wait for the running compositor to exit (logout)
OLD=$(pgrep -x "$COMPOSITOR" | head -1)
if [ -z "$TEST" ] && [ -n "$OLD" ]; then
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
    NEW=$(pgrep -x "$COMPOSITOR" | head -1)
    [ -n "$NEW" ] && [ "$NEW" != "$OLD" -o -n "$TEST" ] && break
    NEW=""
    sleep 1
done
[ -n "$NEW" ] || rollback "no new $COMPOSITOR within ${LOGIN_WAIT}s" session
log "new $COMPOSITOR: PID $NEW. ${WINDOW}s to confirm"

# 3) confirmation window, with a clickable notification once the notification daemon is up
rm -f "$OK"
(
    sleep 8
    [ -n "$TEST" ] && [ -n "$DRY" ] && exit 0
    R=$(timeout "$WINDOW" runuser -u "$USER_NAME" -- env DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$USER_UID/bus \
        notify-send -u critical -a aquamarine -A ok=Keep -w \
        "Keep the new aquamarine?" "Click Keep within ${WINDOW}s, or it rolls back and restarts the session.")
    [ "$R" = ok ] && touch "$OK"
) &

for ((i = 0; i < WINDOW; i++)); do
    if [ -e "$OK" ]; then
        log "confirmed after ${i}s: keeping the new package"
        rm -f "$OK"
        exit 0
    fi
    sleep 1
done
rollback "not confirmed within ${WINDOW}s" session
