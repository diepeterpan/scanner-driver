#!/bin/sh
# Helper script to install compiled driver into host SANE system
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SO_FILE="$SCRIPT_DIR/libsane-ricoh_sp110.so.1.0.0"

if [ ! -f "$SO_FILE" ]; then
    echo "Error: $SO_FILE not found. Run ./build_with_docker.sh first."
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run with sudo or as root:"
    echo "  sudo ./install.sh"
    exit 1
fi

echo "Installing libsane-ricoh_sp110 to /usr/lib/x86_64-linux-gnu/sane/..."
install -d /usr/lib/x86_64-linux-gnu/sane
install -m 755 "$SO_FILE" /usr/lib/x86_64-linux-gnu/sane/
ln -sf libsane-ricoh_sp110.so.1.0.0 /usr/lib/x86_64-linux-gnu/sane/libsane-ricoh_sp110.so.1
ln -sf libsane-ricoh_sp110.so.1 /usr/lib/x86_64-linux-gnu/sane/libsane-ricoh_sp110.so

echo "Installing SANE configuration to /etc/sane.d/ricoh_sp110.conf..."
install -d /etc/sane.d
install -m 644 "$SCRIPT_DIR/../sane.d/ricoh_sp110.conf" /etc/sane.d/

echo "Registering backend in /etc/sane.d/dll.conf and /etc/sane.d/dll.d/..."
touch /etc/sane.d/dll.conf
grep -q "^ricoh_sp110" /etc/sane.d/dll.conf || echo "ricoh_sp110" >> /etc/sane.d/dll.conf
if [ -d "/etc/sane.d/dll.d" ]; then
    echo "ricoh_sp110" > /etc/sane.d/dll.d/ricoh_sp110
fi

echo "Installing udev rules to /etc/udev/rules.d/99-ricoh-sp110.rules..."
install -d /etc/udev/rules.d
install -m 644 "$SCRIPT_DIR/../sane.d/99-ricoh-sp110.rules" /etc/udev/rules.d/

echo "Reloading udev rules..."
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
    echo "Updating standalone test script in $SCRIPT_TARGET_DIR..."
    install -d "$SCRIPT_TARGET_DIR"
    if [ "$SCRIPT_DIR/../python/ricoh_sp110_scanner.py" != "$SCRIPT_TARGET_DIR/ricoh_sp110_scanner.py" ]; then
        cp "$SCRIPT_DIR/../python/ricoh_sp110_scanner.py" "$SCRIPT_TARGET_DIR/"
    fi
    chmod +x "$SCRIPT_TARGET_DIR/ricoh_sp110_scanner.py"
fi

echo "=== Installation complete! ==="
echo "You can now run 'scanimage -L' to verify your scanner."
