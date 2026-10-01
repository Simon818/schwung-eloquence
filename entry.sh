#!/bin/sh
# The "Schwung + Eloquence" boot target. It runs as ableton at every boot,
# before Move starts: if a Schwung update has put its own Flite files back,
# it swaps Eloquence's in again, then starts Schwung exactly as Schwung's own
# boot target does. install.sh runs it with --no-boot to do only the swap.

ELOQ=/data/UserData/eloquence
LIB=/data/UserData/schwung/lib

swap_in() {
    date
    mkdir -p "$ELOQ/orig"
    for ours in "$ELOQ"/payload/libflite*.so.1; do
        [ -f "$ours" ] || continue
        theirs=$LIB/${ours##*/}
        cmp -s "$ours" "$theirs" && continue
        # A file without our marker is Schwung's: keep it for uninstalling.
        if [ -f "$theirs" ] && ! grep -q schwung-eloquence "$theirs"; then
            cp -p "$theirs" "$ELOQ/orig/"
        fi
        # Copy under a temporary name and rename, so it is never half-written.
        cp "$ours" "$theirs.new" && mv -f "$theirs.new" "$theirs" && echo "swapped in ${ours##*/}"
    done
}

swap_in > "$ELOQ/boot.log" 2>&1
[ "$1" = --no-boot ] && exit 0
[ -x /data/UserData/schwung/schwung-entry.sh ] || exec /opt/move/MoveOriginal
exec /data/UserData/schwung/schwung-entry.sh
