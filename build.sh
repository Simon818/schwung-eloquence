#!/usr/bin/env bash
# Build the two files to publish: out/install.sh and out/schwung-eloquence.tar.gz.
#
# Needs Docker. Everything is built in Debian bookworm (see Dockerfile) rather
# than with this computer's compiler, whose newer glibc the Move could not load.
set -euo pipefail
cd "$(dirname "$0")"

[ -f openevv/Makefile ] || git submodule update --init
docker build -q -t schwung-eloquence-build - < Dockerfile > /dev/null

docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/w -w /w schwung-eloquence-build bash -euo pipefail -c '
    echo "Building OpenEVV (the first time takes several minutes)."
    make -C openevv -j"$(nproc)" RULES=c EVVLANG=lang/enus so > /dev/null

    echo "Building libflite.so.1."
    pkg=out/schwung-eloquence
    rm -rf $pkg out/schwung-eloquence.tar.gz out/install.sh
    mkdir -p $pkg/licenses
    # Only the five functions marked EXPORT are visible. PCRE2 is linked in.
    $CC -O2 -std=gnu99 -Wall -Wextra -fPIC -shared -fvisibility=hidden -Iopenevv/include \
        -Wl,-soname,libflite.so.1 -Wl,--exclude-libs,ALL -Wl,-z,defs \
        flite_eci.c -Wl,-Bstatic -lpcre2-8 -Wl,-Bdynamic -ldl -lpthread -lm \
        -o $pkg/libflite.so.1
    # Schwung also links the Flite voice libraries; empty ones carrying our
    # marker stand in for them.
    for voice in cmu_us_kal usenglish cmulex; do
        echo "__attribute__((used)) static const char marker[] = \"schwung-eloquence\";" |
            $CC -x c -shared -fPIC -Wl,-soname,libflite_$voice.so.1 -o $pkg/libflite_$voice.so.1 -
    done
    echo "libflite.so.1 exports:" $(aarch64-linux-gnu-nm -D --defined-only $pkg/libflite.so.1 | cut -d" " -f3)

    cp openevv/build/libeci.so.1 install.sh entry.sh regexp.dic eloquence.example.json VERSION $pkg/
    cp openevv/LICENSE $pkg/licenses/openevv-LICENSE
    cp openevv/NOTICE $pkg/licenses/openevv-NOTICE
    cp /usr/share/doc/libpcre2-8-0/copyright $pkg/licenses/pcre2-LICENCE
    if [ -f LICENSE ]; then cp LICENSE $pkg/licenses/; fi
    tar --owner=0 --group=0 -C out -czf out/schwung-eloquence.tar.gz schwung-eloquence
    rm -rf $pkg
    cp install.sh out/
'
echo "Built version $(cat VERSION). Upload these two files to https://simonj.me/schwung-eloquence/:"
ls -l out/install.sh out/schwung-eloquence.tar.gz
