#!/bin/bash
#
# hack-touchid-volume-mount.sh
#
# Mounts the encrypted HackTouchIDStore volume on demand, pulling its passphrase
# from the System keychain automatically (no human prompt). Meant to be
# called by the CLI/daemon right before touching templates, and paired
# with hack-touchid-volume-unmount.sh right after.
#
# UNIVERSAL VOLUME SUPPORT: a store found on this machine may not be
# the one this install created -- a volume created on one install is
# found the same way on any other, as long as it's attached and the
# enclosing APFS container is unlocked. The one thing that ISN'T
# portable across installs is the System keychain entry (it lives in
# the install's own /Library, not on the HackTouchIDStore volume) --
# so a volume that survives a macOS reinstall or a disk move still
# exists and still has its templates, but the passphrase to decrypt
# it is gone. In that case this script falls back to an interactive
# "adopt" prompt: ask once for the volume's passphrase, and if it
# unlocks successfully, write it back into this install's System
# keychain so every mount after that is automatic again, same as a
# freshly-set-up volume.
#
# DUPLICATE-NAME HANDLING: it's possible to end up with MORE THAN ONE
# volume named "HackTouchIDStore" at once -- e.g. first-time setup got
# re-run (creating a fresh empty one) while an older, real one with
# actual templates is still attached somewhere. diskutil operations
# BY NAME are ambiguous/unreliable once that happens, so every lookup
# below works by disk identifier (e.g. disk3s5), found once via
# list_store_candidates(). With more than one candidate, this script
# tries to figure out which one actually has data (an enrolled finger
# or a stored password) and treats that one as canonical, adopting
# its passphrase into the keychain if needed. It never auto-deletes a
# duplicate -- if more than one candidate turns out to have real data,
# or none can be resolved automatically, it stops and lists everything
# it found so a human decides.
#
# Must run as root (the keychain item and the volume itself are both
# root-only).
#
# Usage: ./hack-touchid-volume-mount.sh
# On success, prints the mount point path on stdout (last line) so a
# caller can capture it: MOUNT_PATH=$(./hack-touchid-volume-mount.sh | tail -1)
# All prompts/diagnostics go to stderr so stdout stays clean for that
# capture, and so they're still visible live to a caller (like the C
# client) that only surfaces captured stdout on failure.

set -e

VOLUME_NAME="HackTouchIDStore"
MOUNT_POINT="/private/var/db/vfsclient_data"
KEYCHAIN_SERVICE="com.mohammad.vfsclient.volume"
KEYCHAIN_ACCOUNT="vfsstore"
ADOPT_MAX_ATTEMPTS=3

if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root." >&2
    exit 1
fi

# Every APFS volume named exactly $VOLUME_NAME, anywhere, one disk
# identifier per line. `diskutil apfs list` interleaves "APFS Volume
# Disk (Role):" (the identifier) with "Name:" for the same volume --
# track the most recent identifier and emit it when its Name matches.
list_store_candidates() {
    diskutil apfs list 2>/dev/null | awk -v want="$VOLUME_NAME" '
        /APFS Volume Disk \(Role\):/ {
            diskid = $0
            sub(/.*APFS Volume Disk \(Role\):[ \t]*/, "", diskid)
            sub(/[ \t].*/, "", diskid)
        }
        /Name:/ {
            name = $0
            sub(/.*Name:[ \t]*/, "", name)
            sub(/ \(Case-insensitive\).*/, "", name)
            gsub(/^[ \t]+|[ \t]+$/, "", name)
            if (name == want && diskid != "") { print diskid; diskid = "" }
        }
    '
}

# Is this disk identifier currently mounted, and if so where? Prints
# the mount point on stdout, nothing if unmounted.
candidate_mount_point() {
    diskutil info "$1" 2>/dev/null | awk -F': +' '/Mount Point:/{print $2; exit}'
}

# Does the volume mounted at this path actually hold data (an
# enrolled finger, or a stored auto-type password)? An empty
# freshly-created volume has neither.
candidate_has_data() {
    local mpath="$1"
    if [ -d "$mpath/fingers" ] && [ -n "$(ls -A "$mpath/fingers" 2>/dev/null)" ]; then
        return 0
    fi
    [ -f "$mpath/password.txt" ]
}

lock_candidate() {
    diskutil apfs lockVolume "$1" >/dev/null 2>&1 || true
}

# NOTE: macOS ships bash 3.2 as /bin/bash (GPLv3 licensing), which has
# no `mapfile`/`readarray` (bash 4+ only) -- a plain read loop instead.
CANDIDATES=()
while IFS= read -r diskid; do
    [ -n "$diskid" ] && CANDIDATES+=("$diskid")
done < <(list_store_candidates)

if [ "${#CANDIDATES[@]}" -eq 0 ]; then
    echo "No HackTouchIDStore volume found and no stored passphrase. Run first-time setup." >&2
    exit 1
fi

PASSPHRASE=$(security find-generic-password \
    -a "$KEYCHAIN_ACCOUNT" \
    -s "$KEYCHAIN_SERVICE" \
    -w \
    /Library/Keychains/System.keychain 2>/dev/null || true)

adopt_one() {
    # Interactively adopts the single disk identifier passed in: prompts
    # for its passphrase, unlocks it at $MOUNT_POINT, and on success
    # saves the passphrase into the System keychain (overwriting
    # whatever was there). Echoes the mount point on success.
    local diskid="$1"

    if [ ! -e /dev/tty ]; then
        echo "No passphrase in this install's System keychain unlocks $diskid, and no" >&2
        echo "terminal is attached to ask for one. Re-run this interactively once." >&2
        return 1
    fi

    echo "Enter the encryption passphrase for $diskid to adopt it -- it will be" >&2
    echo "saved so every mount after this is automatic again." >&2

    local attempt=1 pass
    while [ "$attempt" -le "$ADOPT_MAX_ATTEMPTS" ]; do
        read -r -s -p "Passphrase (attempt $attempt/$ADOPT_MAX_ATTEMPTS): " pass < /dev/tty > /dev/tty 2>&1
        echo "" > /dev/tty
        if echo "$pass" | diskutil apfs unlockVolume "$diskid" -mountpoint "$MOUNT_POINT" -stdinpassphrase >/dev/null 2>&1; then
            security add-generic-password -U \
                -a "$KEYCHAIN_ACCOUNT" \
                -s "$KEYCHAIN_SERVICE" \
                -w "$pass" \
                /Library/Keychains/System.keychain 2>/dev/null
            echo "Adopted $diskid -- passphrase saved to this install's System keychain." >&2
            echo "$MOUNT_POINT"
            return 0
        fi
        echo "That passphrase didn't unlock $diskid." >&2
        attempt=$((attempt + 1))
    done
    return 1
}

finish_mount() {
    # $1 = the path the volume is actually sitting at right now.
    # Re-enforces permissions and prints the final path. CRITICAL:
    # macOS defaults a freshly created/auto-mounted APFS volume's
    # root to root:admin 775 -- any admin-group user (Mohammad's own
    # login included) could read/write it otherwise. Re-enforce
    # root:wheel/700 on EVERY mount, not just once during setup.
    chown root:wheel "$1"
    chmod 700 "$1"
    echo "$1"
}

if [ "${#CANDIDATES[@]}" -eq 1 ]; then
    DISKID="${CANDIDATES[0]}"

    EXISTING_MOUNT=$(candidate_mount_point "$DISKID")
    if [ -n "$EXISTING_MOUNT" ]; then
        finish_mount "$EXISTING_MOUNT"
        exit 0
    fi

    if [ -z "$PASSPHRASE" ]; then
        adopt_one "$DISKID" && exit 0
        echo "Could not adopt the existing volume after $ADOPT_MAX_ATTEMPTS attempts." >&2
        exit 1
    fi

    echo "$PASSPHRASE" | diskutil apfs unlockVolume "$DISKID" -mountpoint "$MOUNT_POINT" -stdinpassphrase
    finish_mount "$MOUNT_POINT"
    exit 0
fi

# --- More than one volume named HackTouchIDStore. Figure out which
# one (if any) has real data, preferring already-mounted candidates
# (cheap to check) before spending an unlock attempt on locked ones. ---
echo "${#CANDIDATES[@]} volumes named \"$VOLUME_NAME\" found: ${CANDIDATES[*]}" >&2

DATA_DISKID=""
DATA_MOUNT=""
EMPTY_CANDIDATES=()

for diskid in "${CANDIDATES[@]}"; do
    mpath=$(candidate_mount_point "$diskid")
    opened_here=0

    if [ -z "$mpath" ]; then
        # Not mounted -- only worth unlocking it to inspect if we
        # actually have a passphrase to try.
        if [ -n "$PASSPHRASE" ] && echo "$PASSPHRASE" | diskutil apfs unlockVolume "$diskid" -mountpoint "$MOUNT_POINT" -stdinpassphrase >/dev/null 2>&1; then
            mpath="$MOUNT_POINT"
            opened_here=1
        else
            echo "  $diskid: locked, stored passphrase did not open it" >&2
            continue
        fi
    fi

    if candidate_has_data "$mpath"; then
        echo "  $diskid: mounted at $mpath, HAS enrolled data" >&2
        if [ -n "$DATA_DISKID" ]; then
            echo "Multiple candidates with real data found ($DATA_DISKID and $diskid)." >&2
            echo "Won't guess -- resolve manually (diskutil apfs deleteVolume on whichever" >&2
            echo "one is stale), then try again." >&2
            exit 1
        fi
        DATA_DISKID="$diskid"
        DATA_MOUNT="$mpath"
    else
        echo "  $diskid: mounted at $mpath, empty" >&2
        EMPTY_CANDIDATES+=("$diskid")
        [ "$opened_here" -eq 1 ] && lock_candidate "$diskid"
    fi
done

if [ -n "$DATA_DISKID" ]; then
    echo "Using $DATA_DISKID as the real store. The other(s) look like stale" >&2
    echo "duplicates -- consider \"diskutil apfs deleteVolume <diskid>\" on them" >&2
    echo "once you've confirmed you don't need them." >&2
    finish_mount "$DATA_MOUNT"
    exit 0
fi

# Nothing with data was found using the stored passphrase. If exactly
# one candidate is still locked (couldn't be opened above), it's the
# most likely home for real templates from a different install --
# offer to adopt that one specifically rather than guessing among
# several.
STILL_LOCKED=()
for diskid in "${CANDIDATES[@]}"; do
    already_empty=0
    for e in "${EMPTY_CANDIDATES[@]}"; do
        [ "$e" = "$diskid" ] && already_empty=1 && break
    done
    [ "$already_empty" -eq 0 ] && STILL_LOCKED+=("$diskid")
done

if [ "${#STILL_LOCKED[@]}" -eq 1 ]; then
    if adopt_one "${STILL_LOCKED[0]}"; then
        exit 0
    fi
    echo "Could not adopt ${STILL_LOCKED[0]} after $ADOPT_MAX_ATTEMPTS attempts." >&2
    exit 1
fi

echo "Could not identify which \"$VOLUME_NAME\" volume has real data. Candidates:" >&2
for diskid in "${CANDIDATES[@]}"; do echo "  $diskid" >&2; done
echo "Resolve manually, then try again." >&2
exit 1
