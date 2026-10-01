# The build environment. Debian bookworm, as Schwung is built, so nothing we
# link needs a newer glibc than the Move's (2.35).
FROM debian:bookworm

RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y \
        gcc-aarch64-linux-gnu make python3 libpcre2-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*

ENV CC=aarch64-linux-gnu-gcc
