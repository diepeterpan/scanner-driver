# Dockerfile for compiling Ricoh SP 110 SANE backend on Debian 64-bit
FROM debian:bookworm-slim

# Install compilation prerequisites
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libusb-1.0-0-dev \
    libsane-dev \
    pkg-config \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Mount current directory at /build and compile
ENTRYPOINT ["make"]
