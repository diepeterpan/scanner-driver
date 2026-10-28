#!/usr/bin/env bash
# Deploy Ricoh SP 110 SANE Driver Bundle to a target Linux machine via SSH / SCP
set -e

# Default variables
SSH_PORT="22"
TARGET_DIR="/tmp/ricoh_sp110_driver_bundle"
RUN_INSTALL=false
SCRIPT_TARGET_DIR=""

usage() {
    cat << EOF
Usage: $0 [OPTIONS] user@target_host

Options:
  -p, --port PORT       Specify SSH port (default: 22)
  -d, --dest DIR        Destination directory on target (default: /tmp/ricoh_sp110_driver_bundle)
  -s, --script-dir DIR  Optional directory on target to install standalone python test script
  -i, --install         Automatically execute install.sh via sudo on target after copying
  -h, --help            Show this help message

Examples:
  # Deploy bundle to remote host:
  $0 user@192.168.1.50

  # Deploy using custom SSH port:
  $0 -p 2222 user@scanner-host.local

  # Deploy and immediately run install on target:
  $0 --install user@192.168.1.50

  # Deploy, install driver, and copy Python test script to /opt/scanners/bin:
  $0 --install -s /opt/scanners/bin user@192.168.1.50
EOF
    exit 1
}

# Parse options
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)
            SSH_PORT="$2"
            shift 2
            ;;
        -d|--dest)
            TARGET_DIR="$2"
            shift 2
            ;;
        -s|--script-dir)
            SCRIPT_TARGET_DIR="$2"
            shift 2
            ;;
        -i|--install)
            RUN_INSTALL=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            TARGET_HOST="$1"
            shift
            ;;
    esac
done

if [ -z "$TARGET_HOST" ]; then
    echo "Error: Target host (user@hostname) is required."
    usage
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRIVER_DIR="$SCRIPT_DIR/driver"
SO_FILE="$DRIVER_DIR/c/libsane-ricoh_sp110.so.1.0.0"

# Check if binary is built
if [ ! -f "$SO_FILE" ]; then
    echo "Compiled library not found at $SO_FILE."
    echo "Running build_with_docker.sh..."
    "$DRIVER_DIR/c/build_with_docker.sh"
fi

# Create a temporary staging directory
STAGE_DIR="$(mktemp -d /tmp/ricoh_pkg_XXXXXX)"
cleanup() {
    rm -rf "$STAGE_DIR"
}
trap cleanup EXIT

echo "--> Preparing deployment bundle in $STAGE_DIR..."
mkdir -p "$STAGE_DIR/bundle"

# Copy library and symlinks
cp -P "$DRIVER_DIR/c"/libsane-ricoh_sp110.so* "$STAGE_DIR/bundle/"

# Copy configurations
cp "$DRIVER_DIR/sane.d/ricoh_sp110.conf" "$STAGE_DIR/bundle/"
cp "$DRIVER_DIR/sane.d/99-ricoh-sp110.rules" "$STAGE_DIR/bundle/"

# Copy python tester utility
cp "$DRIVER_DIR/python/ricoh_sp110_scanner.py" "$STAGE_DIR/bundle/"
chmod +x "$STAGE_DIR/bundle/ricoh_sp110_scanner.py"

# Copy deployment README
cp "$DRIVER_DIR/DEPLOYMENT_README.md" "$STAGE_DIR/bundle/README.md"

# Generate self-contained install.sh for the target machine
cat << 'EOF' > "$STAGE_DIR/bundle/install.sh"
#!/bin/sh
set -e

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This install script must be run as root (e.g. sudo ./install.sh)"
    exit 1
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
SO_FILE="$DIR/libsane-ricoh_sp110.so.1.0.0"

if [ ! -f "$SO_FILE" ]; then
    echo "Error: $SO_FILE not found."
    exit 1
fi

# Determine SANE backend directory
if [ -d "/usr/lib/x86_64-linux-gnu/sane" ]; then
    SANE_LIB_DIR="/usr/lib/x86_64-linux-gnu/sane"
elif [ -d "/usr/lib/sane" ]; then
    SANE_LIB_DIR="/usr/lib/sane"
else
    SANE_LIB_DIR="/usr/lib/x86_64-linux-gnu/sane"
    install -d "$SANE_LIB_DIR"
fi

echo "--> Installing shared library to $SANE_LIB_DIR..."
install -d "$SANE_LIB_DIR"
install -m 755 "$SO_FILE" "$SANE_LIB_DIR/"
ln -sf libsane-ricoh_sp110.so.1.0.0 "$SANE_LIB_DIR/libsane-ricoh_sp110.so.1"
ln -sf libsane-ricoh_sp110.so.1 "$SANE_LIB_DIR/libsane-ricoh_sp110.so"

echo "--> Installing SANE config into /etc/sane.d/ricoh_sp110.conf..."
install -d /etc/sane.d
install -m 644 "$DIR/ricoh_sp110.conf" /etc/sane.d/

echo "--> Registering ricoh_sp110 in /etc/sane.d/dll.conf and /etc/sane.d/dll.d/..."
touch /etc/sane.d/dll.conf
grep -q "^ricoh_sp110" /etc/sane.d/dll.conf || echo "ricoh_sp110" >> /etc/sane.d/dll.conf
if [ -d "/etc/sane.d/dll.d" ]; then
    echo "ricoh_sp110" > /etc/sane.d/dll.d/ricoh_sp110
fi

echo "--> Installing udev rules to /etc/udev/rules.d/99-ricoh-sp110.rules..."
install -d /etc/udev/rules.d
install -m 644 "$DIR/99-ricoh-sp110.rules" /etc/udev/rules.d/

echo "--> Reloading udev rules..."
udevadm control --reload-rules 2>/dev/null || true
udevadm trigger 2>/dev/null || true

# Optional destination for standalone Python test script
SCRIPT_TARGET_DIR="${SCAN_SCRIPT_DIR:-}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        -s|--script-dir)
            SCRIPT_TARGET_DIR="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [-s|--script-dir <DIR>]"
            echo "Options:"
            echo "  -s, --script-dir DIR  Optional directory to install standalone python scanner script"
            echo "  -h, --help            Show this help"
            exit 0
            ;;
        *)
            shift
            ;;
    esac
done

if [ -n "$SCRIPT_TARGET_DIR" ]; then
    echo "--> Installing standalone test script to $SCRIPT_TARGET_DIR..."
    install -d "$SCRIPT_TARGET_DIR"
    if [ "$DIR" != "$SCRIPT_TARGET_DIR" ]; then
        cp "$DIR/ricoh_sp110_scanner.py" "$SCRIPT_TARGET_DIR/"
    fi
    chmod +x "$SCRIPT_TARGET_DIR/ricoh_sp110_scanner.py"
fi

echo "=== Ricoh SP 110 SANE driver successfully installed! ==="
echo "Check scanner recognition with: scanimage -L"
EOF
chmod +x "$STAGE_DIR/bundle/install.sh"

SSH_OPTS=("-o" "StrictHostKeyChecking=accept-new")
if [ -f /etc/ssh/ssh_config.d/20-systemd-ssh-proxy.conf ]; then
    SSH_OPTS+=("-F" "/dev/null")
fi

echo "--> Creating target directory $TARGET_DIR on $TARGET_HOST (port $SSH_PORT)..."
ssh "${SSH_OPTS[@]}" -p "$SSH_PORT" "$TARGET_HOST" "mkdir -p '$TARGET_DIR'"

echo "--> Transferring driver bundle via scp..."
scp "${SSH_OPTS[@]}" -P "$SSH_PORT" -r "$STAGE_DIR/bundle/"* "$TARGET_HOST:$TARGET_DIR/"

echo "--> Files successfully transferred to $TARGET_HOST:$TARGET_DIR!"
echo ""
echo "Remote bundle contents:"
ssh "${SSH_OPTS[@]}" -p "$SSH_PORT" "$TARGET_HOST" "ls -lh '$TARGET_DIR'"

INSTALL_CMD="sudo '$TARGET_DIR/install.sh'"
if [ -n "$SCRIPT_TARGET_DIR" ]; then
    INSTALL_CMD="$INSTALL_CMD -s '$SCRIPT_TARGET_DIR'"
fi

if [ "$RUN_INSTALL" = true ]; then
    echo ""
    echo "--> Running install.sh on target ($TARGET_HOST) via sudo..."
    ssh "${SSH_OPTS[@]}" -t -p "$SSH_PORT" "$TARGET_HOST" "$INSTALL_CMD"
else
    echo ""
    echo "=========================================================================="
    echo "Deployment Complete! To install on the target machine, run:"
    echo "  ssh -p $SSH_PORT $TARGET_HOST"
    echo "  cd $TARGET_DIR"
    if [ -n "$SCRIPT_TARGET_DIR" ]; then
        echo "  sudo ./install.sh -s '$SCRIPT_TARGET_DIR'"
    else
        echo "  sudo ./install.sh"
    fi
    echo "=========================================================================="
fi
