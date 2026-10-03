#!/bin/sh
# Install Eloquence as the voice of Schwung's screen reader.
#
# Run on the Move, logged in over SSH as ableton or root:
#     wget -qO- https://simonj.me/schwung-eloquence/install.sh | sh
# To uninstall, with the copy of this script that installing leaves behind:
#     sh /data/UserData/eloquence/install.sh --uninstall
# To install a schwung-eloquence.tar.gz you already have, name it:
#     sh install.sh schwung-eloquence.tar.gz
# Either way, Move restarts when it is done; --no-restart stops that, for
# example:  | sh -s -- --no-restart
#
# Installing puts Eloquence in /data/UserData/eloquence and swaps it in for
# Schwung's Flite files in /data/UserData/schwung/lib, keeping Schwung's in
# /data/UserData/eloquence/orig. It also adds a "Schwung + Eloquence" boot
# target, made the default, which swaps Eloquence back in after Schwung
# updates. Uninstalling undoes all of that and removes everything in
# /data/UserData/eloquence. Choose the Flite engine in the screen reader
# settings.

URL=https://simonj.me/schwung-eloquence/schwung-eloquence.tar.gz
ELOQ=/data/UserData/eloquence
TARGETS=/data/UserData/boot-targets
SCHWUNG=/data/UserData/schwung
WORK=/data/UserData/.schwung-eloquence-tmp

fail() {
    echo "Error: $*" >&2
    rm -rf "$WORK"
    exit 1
}

# Copy under a temporary name and rename, so no file is ever half-written.
put() {
    cp "$1" "$2.new" && chmod "$3" "$2.new" && mv -f "$2.new" "$2" || fail "could not write $2"
}

set_default() {
    echo "$1" > "$TARGETS/default.new" && mv -f "$TARGETS/default.new" "$TARGETS/default"
}

install() {
    [ -x "$SCHWUNG/schwung-entry.sh" ] || fail "Schwung is not installed. Install Schwung 1.5 or later first."
    # Before 1.5, Schwung calls this missing function, and so crashes Move,
    # whenever the Flite voice's speed or pitch is changed.
    if grep -q tts_save_config "$SCHWUNG/schwung-shim.so" 2>/dev/null; then
        fail "this version of Schwung crashes Move when the Flite voice's speed or pitch is changed. Update Schwung to 1.5 or later first."
    fi
    rm -rf "$WORK"
    mkdir -p "$WORK" || fail "could not create $WORK"

    if [ -z "$archive" ]; then
        archive=$WORK/download.tar.gz
        echo "Downloading Eloquence."
        wget -q -O "$archive" "$URL" || fail "the download failed"
        echo "Download finished."
    fi
    # gzip checks its own contents, so a damaged download stops here.
    tar -xzf "$archive" -C "$WORK" 2>/dev/null || fail "the archive is damaged or incomplete. Please try again."
    new=$WORK/schwung-eloquence
    version=$(cat "$new/VERSION")

    mkdir -p "$ELOQ/payload" "$ELOQ/dict" "$TARGETS/eloquence" || fail "could not create $ELOQ"
    put "$new/libeci.so.1" "$ELOQ/libeci.so.1" 755
    for f in "$new"/libflite*.so.1; do
        put "$f" "$ELOQ/payload/${f##*/}" 755
    done
    put "$new/install.sh" "$ELOQ/install.sh" 755
    put "$new/regexp.dic" "$ELOQ/dict/regexp.dic" 644
    put "$new/eloquence.example.json" "$ELOQ/eloquence.example.json" 644
    rm -rf "$ELOQ/licenses" && cp -R "$new/licenses" "$ELOQ/licenses"
    put "$new/entry.sh" "$TARGETS/eloquence/entry.sh" 755
    cat > "$WORK/boot.json" <<EOF
{
  "name": "Schwung + Eloquence",
  "exec": "$TARGETS/eloquence/entry.sh",
  "version": "$version",
  "author": "schwung-eloquence"
}
EOF
    put "$WORK/boot.json" "$TARGETS/eloquence/boot.json" 644
    set_default eloquence || fail "could not make Schwung + Eloquence the default boot target"

    # The same swap the boot target does at every boot.
    "$TARGETS/eloquence/entry.sh" --no-boot || fail "could not swap in Eloquence's Flite files"

    if [ "$(id -u)" = 0 ]; then
        chown -R ableton:users "$ELOQ" "$TARGETS/eloquence" "$TARGETS/default" "$SCHWUNG"/lib/libflite*.so.1
    fi
    rm -rf "$WORK"
    echo "Eloquence $version is installed and has replaced the Flite voice."
}

uninstall() {
    for f in "$ELOQ"/orig/libflite*.so.1; do
        [ -f "$f" ] && put "$f" "$SCHWUNG/lib/${f##*/}" 644
    done
    rm -rf "$TARGETS/eloquence"
    [ "$(cat "$TARGETS/default" 2>/dev/null)" = eloquence ] && set_default schwung
    rm -rf "$ELOQ"
    echo "Eloquence is uninstalled and Schwung's own Flite voice is back."
}

restart_move() {
    if [ ! -x "$SCHWUNG/restart-move.sh" ]; then
        echo "Restart your Move for the change to take effect."
        return
    fi
    echo "Restarting Move."
    # Schwung's own restart, which stops every Move process; the init
    # script's stop leaves Move running. As ableton, so Move does not come
    # back running as root.
    if [ "$(id -u)" = 0 ]; then
        su ableton -s /bin/sh -c "$SCHWUNG/restart-move.sh"
    else
        "$SCHWUNG/restart-move.sh"
    fi
}

main() {
    action=install
    restart=yes
    archive=
    for arg in "$@"; do
        case "$arg" in
        --uninstall)  action=uninstall ;;
        --no-restart) restart=no ;;
        -*) fail "unknown option $arg. The options are --uninstall and --no-restart." ;;
        *)  archive=$arg ;;
        esac
    done

    $action
    if [ "$restart" = yes ]; then
        restart_move
    else
        echo "Restart your Move for the change to take effect."
    fi
}

# Nothing runs until this last line has arrived, so an interrupted download
# of this script does nothing at all.
main "$@"
