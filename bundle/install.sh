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
