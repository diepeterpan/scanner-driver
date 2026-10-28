# Guide: Building, Installing, and Testing the Ricoh SP 110 Series SANE Driver

This guide explains how to install the necessary build tools, compile the SANE 64-bit backend library (`libsane-ricoh_sp110.so.1`), configure system permissions, and use the Python tester app.

---

## 1. Supported Models

| Model | USB VID / PID | Capabilities |
|---|---|---|
| **Ricoh SP 112SU** | `0x05CA` / `0x0448` | 3-in-1 (Flatbed only) |
| **Ricoh SP 112SF** | `0x05CA` / `0x0449` | 4-in-1 (Flatbed + Automatic Document Feeder) |

Both models share the identical command protocol, with model-aware filtering for ADF functionality.

---

## 2. Compiling the C SANE Backend (`.so`)

### Option A: Using Docker (Recommended - Zero Host Dependencies)
A dedicated Debian `bookworm-slim` Docker builder image (`ricoh-sane-builder:latest`) has been created. You do **not** need to install `gcc`, `make`, or development headers on your host machine.

To compile or recompile in the future, simply run:
```bash
cd driver/c
./build_with_docker.sh
```
This mounts the directory inside the Debian container, compiles with matching host UID/GID permissions, and outputs:
- `libsane-ricoh_sp110.so.1.0.0`
- `libsane-ricoh_sp110.so.1` (symlink)
- `libsane-ricoh_sp110.so` (symlink)

### Option B: Compiling Directly on Host
If you prefer building without Docker:
```bash
sudo apt update && sudo apt install -y build-essential libusb-1.0-0-dev libsane-dev
cd driver/c
make
```

---

## 3. Installing the Driver into SANE

Once compiled (via Docker or on host), install it into your system using the included installer script:
```bash
cd driver/c
sudo ./install.sh
```

To optionally install the standalone Python test script to a custom directory (e.g., `/opt/cups/scans` or web scan UI scripts):
```bash
sudo ./install.sh -s /opt/cups/scans
# or via environment variable:
# SCAN_SCRIPT_DIR=/opt/cups/scans sudo -E ./install.sh
```

**What the installer does:**
1. Copies `libsane-ricoh_sp110.so.1.0.0` to `/usr/lib/x86_64-linux-gnu/sane/`.
2. Creates the `/usr/lib/x86_64-linux-gnu/sane/libsane-ricoh_sp110.so.1` library link.
3. Installs the backend configuration `/etc/sane.d/ricoh_sp110.conf`.
4. Registers `ricoh_sp110` in `/etc/sane.d/dll.conf` so SANE dynamically loads it.
5. Installs `/etc/udev/rules.d/99-ricoh-sp110.rules` and triggers `udevadm` so normal users can scan without needing `sudo`.
6. If `-s <DIR>` or `SCAN_SCRIPT_DIR` was specified, installs executable `ricoh_sp110_scanner.py` into that target directory.

---

## 4. Testing with SANE
```bash
# Verify the scanner is detected by SANE
scanimage -L

# Query device capabilities and options
scanimage --device "ricoh_sp110:..." -A

# Perform a test scan to PNG
scanimage --device "ricoh_sp110:..." --format=png --output=scan.png
```

---

## 3. Python Tester App Instructions

The Python tester utility [`driver/python/ricoh_sp110_scanner.py`](driver/python/ricoh_sp110_scanner.py) communicates directly with the scanner over USB using system `libusb-1.0` via `ctypes`. It has **no external pip dependencies**.

### Permissions / Execution Note
Make sure your user account has permission to read/write USB devices (e.g. member of group `scanner` or `lp`, or test with `sudo` before udev rules are loaded).

### Usage Commands

#### 1. Check Scanner Status & Sensors
Interrogates the scanner hardware, warm-up state, home position, cover sensor, and ADF paper sensor:
```bash
./driver/python/ricoh_sp110_scanner.py --status
```
Example Output:
```text
Connected to Ricoh SP 112SF (PID 0x0449)
Scanner Status:
  Ready:             True
  ADF Paper Present: False
  Cover Open:        False
  Paper Jam:         False
  Home Position:     True
  Raw Bytes:         ['0x0', '0x0', '0x0', ...]
```

#### 2. Flatbed Scan (Default)
Scans an A4/Letter document from the glass flatbed at 300 DPI in 24-bit RGB color:
```bash
./driver/python/ricoh_sp110_scanner.py --scan scan_flatbed.pnm --dpi 300 --mode color --source flatbed
```

#### 3. Automatic Document Feeder (ADF) Scan (SP 112SF only)
Feeds and scans a sheet from the top ADF tray:
```bash
./driver/python/ricoh_sp110_scanner.py --scan scan_adf.pnm --dpi 300 --mode color --source adf
```
*(Note: If no paper is placed in the tray, it safely refuses to scan and alerts you).*

#### 4. Grayscale or Fast Lineart Scan
```bash
# Grayscale 300 DPI
./driver/python/ricoh_sp110_scanner.py --scan doc_gray.pnm --dpi 300 --mode gray

# High resolution 600 DPI
./driver/python/ricoh_sp110_scanner.py --scan highres.pnm --dpi 600 --mode color
```

#### 5. Converting Output PNM Images
The output `.pnm` file is standard uncompressed Netpbm format (P5 grayscale / P6 RGB color), supported natively by GIMP, ImageMagick, and viewers:
```bash
# Convert to PNG using ImageMagick
convert scan_flatbed.pnm scan_flatbed.png

# Or view directly with any viewer (e.g. eog, display)
eog scan_flatbed.pnm
```
