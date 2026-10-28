# Ricoh SP 110 Series SANE Driver Deployment Package
Supports **Ricoh SP 112SF** (4-in-1 ADF) and **Ricoh SP 112SU** (3-in-1 Flatbed).

## Package Contents
- `libsane-ricoh_sp110.so.1.0.0`: Precompiled 64-bit SANE backend shared library.
- `libsane-ricoh_sp110.so.1`: Library symlink.
- `ricoh_sp110.conf`: SANE backend configuration file (`/etc/sane.d/`).
- `99-ricoh-sp110.rules`: Udev permission rules for USB device access (`/etc/udev/rules.d/`).
- `install.sh`: Automated installation script.
- `ricoh_sp110_scanner.py`: Standalone Python CLI tester utility (uses system `libusb-1.0`, zero pip dependencies).

---

## Installation Instructions (Target Machine)

Run the included install script with root privileges:

```bash
cd ricoh_sp110_driver_bundle
sudo ./install.sh
```

To optionally install the standalone Python test script to a custom directory (e.g., `/opt/cups/scans` or web scan UI scripts):
```bash
sudo ./install.sh -s /opt/cups/scans
# or via environment variable:
# SCAN_SCRIPT_DIR=/opt/cups/scans sudo -E ./install.sh
```

### What `install.sh` Does:
1. Copies `libsane-ricoh_sp110.so.1.0.0` into `/usr/lib/x86_64-linux-gnu/sane/` (or `/usr/lib/sane/`).
2. Creates the `libsane-ricoh_sp110.so.1` symlink.
3. Installs `/etc/sane.d/ricoh_sp110.conf`.
4. Appends `ricoh_sp110` to `/etc/sane.d/dll.conf` so SANE automatically discovers it.
5. Installs `/etc/udev/rules.d/99-ricoh-sp110.rules` and triggers `udevadm` so non-root users can scan.
6. If `-s <DIR>` or `SCAN_SCRIPT_DIR` was specified, installs executable `ricoh_sp110_scanner.py` into that target directory.

---

## Verifying and Using the Scanner

### 1. With SANE Tools (`scanimage`, Simple Scan, XSane)
```bash
# Check if SANE detects the scanner
scanimage -L

# Query options and capabilities (including paper-size: A4, A5, A6, B5, B6, Legal, Letter)
scanimage -d ricoh_sp110 -A

# 100 DPI Color Flatbed scan (subsampled 2:1 on the fly)
scanimage -d ricoh_sp110 --paper-size A4 --resolution 100 --mode Color > test_100.pnm

# A4 Color Flatbed scan to PNM
scanimage -d ricoh_sp110 --paper-size A4 --resolution 300 --mode Color > test_a4.pnm

# A5 Grayscale Flatbed scan to PNM
scanimage -d ricoh_sp110 --paper-size A5 --resolution 300 --mode Gray > test_a5.pnm

# Legal scan from ADF (SP 112SF only)
scanimage -d ricoh_sp110 --source ADF --paper-size Legal --resolution 300 --mode Gray > test_legal.pnm

# 300 DPI Monochrome (1-bit Lineart)
scanimage -d ricoh_sp110 --resolution 300 --mode Lineart > test_lineart.pnm
```

### 2. With the Standalone Python Tester App
```bash
# Check hardware status & paper sensors
python3 ./ricoh_sp110_scanner.py --status

# Return carriage to home parking position and clear state (software reset)
python3 ./ricoh_sp110_scanner.py --cancel

# 300 DPI Color scan
python3 ./ricoh_sp110_scanner.py --scan test_color.pnm --dpi 300 --mode color --source flatbed

# 300 DPI Gray scan
python3 ./ricoh_sp110_scanner.py --scan test_gray.pnm --dpi 300 --mode gray --source flatbed

# ADF scan (SP 112SF only)
python3 ./ricoh_sp110_scanner.py --scan test_adf.pnm --dpi 300 --mode gray --source adf
```
