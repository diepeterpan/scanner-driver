#!/usr/bin/env python3
"""
Standalone Python driver and test utility for Ricoh SP 112SF / SP 112SU scanners.
Interacts directly with the device via libusb-1.0 using ctypes (no external pip dependencies).
"""

import sys
import os
import time
import struct
import argparse
import ctypes
import ctypes.util

# ---------------------------------------------------------------------------
# libusb-1.0 ctypes wrapper
# ---------------------------------------------------------------------------

class libusb_version(ctypes.Structure):
    _fields_ = [
        ("major", ctypes.c_uint16),
        ("minor", ctypes.c_uint16),
        ("micro", ctypes.c_uint16),
        ("nano", ctypes.c_uint16),
        ("rc", ctypes.c_char_p),
        ("describe", ctypes.c_char_p),
    ]

class libusb_device_descriptor(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_uint8),
        ("bDescriptorType", ctypes.c_uint8),
        ("bcdUSB", ctypes.c_uint16),
        ("bDeviceClass", ctypes.c_uint8),
        ("bDeviceSubClass", ctypes.c_uint8),
        ("bDeviceProtocol", ctypes.c_uint8),
        ("bMaxPacketSize0", ctypes.c_uint8),
        ("idVendor", ctypes.c_uint16),
        ("idProduct", ctypes.c_uint16),
        ("bcdDevice", ctypes.c_uint16),
        ("iManufacturer", ctypes.c_uint8),
        ("iProduct", ctypes.c_uint8),
        ("iSerialNumber", ctypes.c_uint8),
        ("bNumConfigurations", ctypes.c_uint8),
    ]

# Opaque pointer types
libusb_context_p = ctypes.c_void_p
libusb_device_p = ctypes.c_void_p
libusb_device_handle_p = ctypes.c_void_p
libusb_device_p_p = ctypes.POINTER(libusb_device_p)

# Load library
_libusb_path = ctypes.util.find_library("usb-1.0") or "libusb-1.0.so.0"
_lib = ctypes.CDLL(_libusb_path)

# Function prototypes
_lib.libusb_init.argtypes = [ctypes.POINTER(libusb_context_p)]
_lib.libusb_init.restype = ctypes.c_int

_lib.libusb_exit.argtypes = [libusb_context_p]
_lib.libusb_exit.restype = None

_lib.libusb_get_device_list.argtypes = [libusb_context_p, ctypes.POINTER(libusb_device_p_p)]
_lib.libusb_get_device_list.restype = ctypes.c_ssize_t

_lib.libusb_free_device_list.argtypes = [libusb_device_p_p, ctypes.c_int]
_lib.libusb_free_device_list.restype = None

_lib.libusb_get_device_descriptor.argtypes = [libusb_device_p, ctypes.POINTER(libusb_device_descriptor)]
_lib.libusb_get_device_descriptor.restype = ctypes.c_int

_lib.libusb_open.argtypes = [libusb_device_p, ctypes.POINTER(libusb_device_handle_p)]
_lib.libusb_open.restype = ctypes.c_int

_lib.libusb_close.argtypes = [libusb_device_handle_p]
_lib.libusb_close.restype = None

_lib.libusb_claim_interface.argtypes = [libusb_device_handle_p, ctypes.c_int]
_lib.libusb_claim_interface.restype = ctypes.c_int

_lib.libusb_release_interface.argtypes = [libusb_device_handle_p, ctypes.c_int]
_lib.libusb_release_interface.restype = ctypes.c_int

_lib.libusb_kernel_driver_active.argtypes = [libusb_device_handle_p, ctypes.c_int]
_lib.libusb_kernel_driver_active.restype = ctypes.c_int

_lib.libusb_detach_kernel_driver.argtypes = [libusb_device_handle_p, ctypes.c_int]
_lib.libusb_detach_kernel_driver.restype = ctypes.c_int

_lib.libusb_bulk_transfer.argtypes = [
    libusb_device_handle_p,
    ctypes.c_uint8,
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_int),
    ctypes.c_uint,
]
_lib.libusb_bulk_transfer.restype = ctypes.c_int

_lib.libusb_control_transfer.argtypes = [
    libusb_device_handle_p,
    ctypes.c_uint8,   # bmRequestType
    ctypes.c_uint8,   # bRequest
    ctypes.c_uint16,  # wValue
    ctypes.c_uint16,  # wIndex
    ctypes.c_char_p,  # data
    ctypes.c_uint16,  # wLength
    ctypes.c_uint,    # timeout
]
_lib.libusb_control_transfer.restype = ctypes.c_int

_lib.libusb_clear_halt.argtypes = [libusb_device_handle_p, ctypes.c_uint8]
_lib.libusb_clear_halt.restype = ctypes.c_int

_lib.libusb_set_interface_alt_setting.argtypes = [libusb_device_handle_p, ctypes.c_int, ctypes.c_int]
_lib.libusb_set_interface_alt_setting.restype = ctypes.c_int

_lib.libusb_reset_device.argtypes = [libusb_device_handle_p]
_lib.libusb_reset_device.restype = ctypes.c_int

# ---------------------------------------------------------------------------
# Device Constants & Protocol Parameters
# ---------------------------------------------------------------------------

RICOH_VID = 0x05CA
PID_SP112SU = 0x0448   # 3-in-1 (Flatbed only)
PID_SP112SF = 0x0449   # 4-in-1 (Flatbed + ADF)

INTERFACE_SCANNER = 0
ENDPOINT_BULK_OUT = 0x03   # EP 3 OUT: Command writes (64-byte framing packets)
ENDPOINT_RESP_IN  = 0x84   # EP 4 IN: Command responses, Status (0x0D), Homing ACK (0x09), Caps (0x0B)
ENDPOINT_RASTER_IN = 0x85  # EP 5 IN: High-speed raster image streaming
ENDPOINT_BULK_IN  = 0x84   # Legacy alias
ENDPOINT_BULK_ALT = 0x85   # Legacy alias

RESOLUTION_CODES = {
    200: 0x01,
    300: 0x01,
    600: 0x02,
}

SCAN_MODES = {
    'mono': 0x00,      # 1-bit monochrome
    'gray': 0x02,      # 8-bit grayscale
    'color': 0x03,     # 24-bit RGB
}

SOURCES = {
    'flatbed': 0x00,
    'adf': 0x01,
}

PAPER_SIZES = {
    'a4': (210.0, 297.0),
    'a5': (148.0, 210.0),
    'a6': (105.0, 148.0),
    'b5': (176.0, 250.0), # ISO B5
    'b6': (125.0, 176.0), # ISO B6
    'legal': (215.9, 355.6),
    'letter': (215.9, 279.4),
    'max': (215.9, 297.0),
}

CMD_PREAMBLE         = 0x03

CMD_SUBTYPE_STATUS   = 0x0D
CMD_RESP_LEN_STATUS  = 11

CMD_SUBTYPE_GEOMETRY = 0x0C
CMD_RESP_LEN_GEOM    = 17

CMD_SUBTYPE_CANCEL   = 0x09
CMD_RESP_LEN_CANCEL  = 1

CMD_SUBTYPE_CAPS     = 0x0B
CMD_RESP_LEN_CAPS    = 8

CMD_SUBTYPE_LAMP     = 0x08
CMD_RESP_LEN_LAMP    = 4

CMD_SUBTYPE_START    = 0x0E
CMD_RESP_LEN_START   = 4

CMD_SUBTYPE_END_SCAN = 0x0A
CMD_RESP_LEN_END_SCAN = 0

CMD_SUBTYPE_WINDOW   = 0x0A  # Legacy alias
CMD_RESP_LEN_WINDOW  = 10

class RicohScannerUSB:
    def __init__(self, target_pid=None):
        self.ctx = libusb_context_p()
        ret = _lib.libusb_init(ctypes.byref(self.ctx))
        if ret != 0:
            raise RuntimeError(f"Failed to initialize libusb: error {ret}")

        self.handle = None
        self.pid = None
        self.model_name = ""
        self._find_and_open_device(target_pid)

    def _find_and_open_device(self, target_pid=None):
        devs = libusb_device_p_p()
        cnt = _lib.libusb_get_device_list(self.ctx, ctypes.byref(devs))
        if cnt < 0:
            raise RuntimeError("Failed to enumerate USB devices")

        found_dev = None
        found_pid = None

        try:
            for i in range(cnt):
                dev = devs[i]
                desc = libusb_device_descriptor()
                if _lib.libusb_get_device_descriptor(dev, ctypes.byref(desc)) == 0:
                    if desc.idVendor == RICOH_VID:
                        if target_pid and desc.idProduct == target_pid:
                            found_dev = dev
                            found_pid = desc.idProduct
                            break
                        elif not target_pid and desc.idProduct in (PID_SP112SF, PID_SP112SU):
                            found_dev = dev
                            found_pid = desc.idProduct
                            break

            if not found_dev:
                raise RuntimeError("No compatible Ricoh SP 110 Series scanner found (VID 0x05CA, PID 0x0448/0x0449)")

            self.handle = libusb_device_handle_p()
            ret = _lib.libusb_open(found_dev, ctypes.byref(self.handle))
            if ret != 0:
                raise PermissionError(
                    f"Found scanner (PID 0x{found_pid:04x}) but failed to open device (code {ret}). "
                    "Make sure you have permission or install udev rules."
                )

            self.pid = found_pid
            self.model_name = "Ricoh SP 112SF" if self.pid == PID_SP112SF else "Ricoh SP 112SU"

            # Detach kernel driver if bound to scanner interface
            if _lib.libusb_kernel_driver_active(self.handle, INTERFACE_SCANNER) == 1:
                _lib.libusb_detach_kernel_driver(self.handle, INTERFACE_SCANNER)

            # Claim scanner interface
            ret = _lib.libusb_claim_interface(self.handle, INTERFACE_SCANNER)
            if ret != 0:
                raise RuntimeError(f"Failed to claim scanner USB interface {INTERFACE_SCANNER}: code {ret}")

            # Clear any stale halt condition on bulk endpoints
            self.clear_halt()

        finally:
            _lib.libusb_free_device_list(devs, 1)

    def clear_halt(self):
        """Clears stall/halt conditions and flushes FIFOs on scanner interface."""
        if self.handle:
            # 1. Reset alternate setting to 0 to flush interface FIFOs and data toggles
            _lib.libusb_set_interface_alt_setting(self.handle, INTERFACE_SCANNER, 0)
            # 2. Clear endpoint stalls
            _lib.libusb_clear_halt(self.handle, ENDPOINT_BULK_OUT)
            _lib.libusb_clear_halt(self.handle, ENDPOINT_BULK_IN)
            _lib.libusb_clear_halt(self.handle, ENDPOINT_BULK_ALT)
            # 3. Drain any stale bytes sitting in Bulk IN FIFO
            self._drain_bulk_in()

    def _drain_bulk_in(self):
        """Drains any residual data sitting in Bulk IN FIFO to relieve USB backpressure."""
        if not self.handle:
            return
        buf = ctypes.create_string_buffer(4096)
        transferred = ctypes.c_int()
        for ep in (ENDPOINT_BULK_IN, ENDPOINT_BULK_ALT):
            while True:
                ret = _lib.libusb_bulk_transfer(self.handle, ep, buf, 4096, ctypes.byref(transferred), 20)
                if ret != 0 or transferred.value == 0:
                    break

    def reset_device(self):
        """Performs a software reset via endpoint halt clearance and carriage homing (Command 3).
        Avoids USBDEVFS_RESET which crashes/disconnects the USB PHY on composite multifunction hardware."""
        if self.handle:
            print(f"[{self.model_name}] Performing software reset (clearing endpoint stalls & homing carriage)...")
            self.clear_halt()
            try:
                ack = self.cancel_scan(verbose=True)
                self.clear_halt()
                if ack is not None:
                    print(f"[{self.model_name}] Software reset successful! Carriage homed with ACK: 0x{ack:02x}.")
                else:
                    print(f"[{self.model_name}] Software reset packet transmitted.")
            except Exception as e:
                print(f"[{self.model_name}] Reset warning: {e}")

    def close(self):
        if self.handle:
            _lib.libusb_release_interface(self.handle, INTERFACE_SCANNER)
            _lib.libusb_close(self.handle)
            self.handle = None
        if self.ctx:
            _lib.libusb_exit(self.ctx)
            self.ctx = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def _bulk_write(self, endpoint, data: bytes, timeout_ms=5000):
        transferred = ctypes.c_int()
        buf = ctypes.create_string_buffer(data, len(data))
        ret = _lib.libusb_bulk_transfer(
            self.handle,
            endpoint,
            buf,
            len(data),
            ctypes.byref(transferred),
            timeout_ms
        )
        if ret != 0:
            raise IOError(f"USB bulk write failed: error {ret}")
        return transferred.value

    def _bulk_read(self, endpoint, length: int, timeout_ms=5000) -> bytes:
        raw_buf = ctypes.create_string_buffer(length)
        transferred = ctypes.c_int()
        ret = _lib.libusb_bulk_transfer(
            self.handle,
            endpoint,
            raw_buf,
            length,
            ctypes.byref(transferred),
            timeout_ms
        )
        if ret != 0:
            raise IOError(f"USB bulk read failed: error {ret}")
        return raw_buf.raw[:transferred.value]

    def _bulk_read_resp(self, length: int, timeout_ms=3000) -> bytes:
        """Reads from primary endpoint (0x84) with fallback to alternate (0x85)."""
        try:
            return self._bulk_read(ENDPOINT_BULK_IN, length, timeout_ms=timeout_ms)
        except IOError as e1:
            try:
                alt_timeout = min(timeout_ms, 5000)
                return self._bulk_read(ENDPOINT_BULK_ALT, length, timeout_ms=alt_timeout)
            except IOError:
                raise e1

    def build_packet(self, subtype: int, resp_len: int, payload: bytes = b'') -> bytes:
        """Constructs the standard fixed 64-byte framing packet."""
        pkt = bytearray(64)
        pkt[0] = CMD_PREAMBLE
        pkt[1] = subtype & 0xFF
        pkt[2] = resp_len & 0xFF
        if payload:
            copy_len = min(len(payload), 64 - 6)
            pkt[6:6 + copy_len] = payload[:copy_len]
        return bytes(pkt)

    def query_status(self) -> dict:
        """Sends CMD_ADFhasPaper (Subtype 0x0D) and returns parsed sensor states."""
        pkt = self.build_packet(CMD_SUBTYPE_STATUS, CMD_RESP_LEN_STATUS)
        self._bulk_write(ENDPOINT_BULK_OUT, pkt)
        resp = self._bulk_read_resp(CMD_RESP_LEN_STATUS, timeout_ms=5000)
        if len(resp) < 1:
            raise IOError("Empty response for status query")

        # Byte 0 & 0x01: ADF hardware installed; Byte 1 & 0x01: Paper in ADF tray
        adf_installed = bool(resp[0] & 0x01)
        has_paper = bool(resp[1] & 0x01) if len(resp) > 1 else False
        cover_open = bool(resp[1] & 0x02) if len(resp) > 1 else False
        paper_jam = bool(resp[1] & 0x04) if len(resp) > 1 else False
        home_pos = bool(resp[2] == 0) if len(resp) > 2 else True

        return {
            'ready': True,
            'adf_unit_installed': adf_installed,
            'adf_has_paper': has_paper,
            'cover_open': cover_open,
            'paper_jam': paper_jam,
            'home_position': home_pos,
            'raw': list(resp),
        }

    def configure_scan(self, dpi=300, mode='color', source='flatbed', x=0, y=0, width_px=2550, height_px=3300):
        """Builds 17-byte parameter block and sends geometry configuration (Write-Only)."""
        # Hardware only supports 300 DPI (0x01) and 600 DPI (0x02) optical stepping
        hw_dpi = 600 if dpi >= 600 else 300
        res_code = 0x02 if hw_dpi >= 600 else 0x01
        # Hardware ScanMode: 0x03 for color, 0x02 for gray/lineart
        mode_code = 0x03 if mode == 'color' else 0x02
        src_code = SOURCES.get(source, 0x00)

        # Scale coordinates to 600 DPI base units per original driver:
        # coord_600 = (coord_dpi * 600) / requested_dpi
        x_600 = (x * 600) // dpi
        y_600 = (y * 600) // dpi
        w_600 = (width_px * 600) // dpi
        h_600 = (height_px * 600) // dpi

        # Align height to even number for 600 DPI ADF
        if hw_dpi >= 600 and src_code == 1 and (h_600 & 1):
            h_600 += 1

        param_payload = struct.pack(
            '<BBBBHHHHB4s',
            res_code,
            mode_code,
            0x00,           # Brightness
            0x00,           # Papersize / Threshold
            x_600,          # StartX (600 DPI)
            y_600,          # StartY (600 DPI)
            w_600,          # Width (600 DPI)
            h_600,          # Height (600 DPI)
            src_code,       # 0 = Flatbed, 1 = ADF
            b'\x00' * 4     # Padding (X/Y resolution for custom DPI)
        )

        pkt = self.build_packet(CMD_SUBTYPE_GEOMETRY, CMD_RESP_LEN_GEOM, param_payload)
        self._bulk_write(ENDPOINT_BULK_OUT, pkt)

    def get_capabilities(self) -> dict:
        """Sends CMD_GetCapabilities (Command 4, Subtype 0x0B, len 8) and reads 8-byte response.
        Returns logic and actual dimensions confirmed by the hardware optical engine."""
        pkt = self.build_packet(CMD_SUBTYPE_CAPS, CMD_RESP_LEN_CAPS)
        self._bulk_write(ENDPOINT_BULK_OUT, pkt)
        resp = self._bulk_read_resp(CMD_RESP_LEN_CAPS, timeout_ms=5000)
        if len(resp) < 8:
            raise IOError(f"Invalid capabilities response length: {len(resp)} bytes")
        logic_w = resp[0] | (resp[1] << 8)
        logic_h = resp[2] | (resp[3] << 8)
        actual_w = resp[4] | (resp[5] << 8)
        actual_h = resp[6] | (resp[7] << 8)
        return {
            'logic_width': logic_w,
            'logic_height': logic_h,
            'actual_width': actual_w,
            'actual_height': actual_h,
            'raw': list(resp)
        }

    def lamp_control(self, total_bytes: int = 0):
        """Sends lamp control / calibration command (Subtype 0x08, Write-Only).
        Payload: 4-byte total scan bytes + 0x01 flag."""
        payload = struct.pack('<IB', total_bytes & 0xFFFFFFFF, 0x01)
        pkt = self.build_packet(CMD_SUBTYPE_LAMP, CMD_RESP_LEN_LAMP, payload)
        self._bulk_write(ENDPOINT_BULK_OUT, pkt)

    def start_scan(self, chunk_bytes: int):
        """Sends scan trigger / chunk request command (Subtype 0x0E, Write-Only).
        Payload: 4-byte requested chunk byte count."""
        payload = struct.pack('<I', chunk_bytes & 0xFFFFFFFF)
        pkt = self.build_packet(CMD_SUBTYPE_START, CMD_RESP_LEN_START, payload)
        self._bulk_write(ENDPOINT_BULK_OUT, pkt)

    def end_scan(self):
        """Sends CMD_EndScan (Subtype 0x0A, len 0, Write-Only) to close active scan session."""
        pkt = self.build_packet(CMD_SUBTYPE_END_SCAN, CMD_RESP_LEN_END_SCAN)
        self._bulk_write(ENDPOINT_BULK_OUT, pkt)

    def cancel_scan(self, verbose: bool = False) -> int:
        """Sends abort / homing command (Subtype 0x09) and reads 1-byte ACK (0x00).
        Returns the ACK byte value."""
        self.clear_halt()
        pkt = self.build_packet(CMD_SUBTYPE_CANCEL, CMD_RESP_LEN_CANCEL)
        if verbose:
            print(f"[{self.model_name}] Sending CMD_CancelScan (0x09) to park/home carriage...")
        self._bulk_write(ENDPOINT_BULK_OUT, pkt, timeout_ms=3000)
        resp = self._bulk_read_resp(CMD_RESP_LEN_CANCEL, timeout_ms=3000)
        if not resp:
            raise IOError("No response received for CMD_CancelScan")
        ack = resp[0]
        if verbose:
            print(f"[{self.model_name}] Carriage homed successfully! ACK byte: 0x{ack:02x}")
        return ack

    def close_session(self):
        """Closes scan session with CMD_EndScan (0x0A) and returns carriage home with CMD_CancelScan (0x09)."""
        try:
            self.end_scan()
        except Exception:
            pass
        try:
            self.cancel_scan()
        except Exception:
            pass

    def scan_to_pnm(self, output_path, dpi=300, mode='color', source='flatbed', width_px=2550, height_px=3300):
        """Executes full scan sequence according to the hardware protocol and saves to PNM."""
        downscale_100 = (dpi == 100)
        downscale_200 = (dpi == 200)
        # Hardware only supports 300 DPI (0x01) and 600 DPI (0x02) optical stepping
        hw_dpi = 600 if dpi >= 600 else 300
        w_600 = (width_px * 600) // dpi
        h_600 = (height_px * 600) // dpi
        hw_w = (w_600 * hw_dpi) // 600
        hw_h = (h_600 * hw_dpi) // 600

        # Clear endpoints to avoid stale stalls from previous runs
        self.clear_halt()

        # Step 1: Device status check (Command 1, 0x0D, len 11)
        print(f"[{self.model_name}] Step 1: Querying device status...")
        status = self.query_status()
        print(f"Status: ready={status['ready']}, ADF unit={status['adf_unit_installed']}, ADF paper={status['adf_has_paper']}, cover_open={status['cover_open']}")

        if source == 'adf':
            if self.pid == PID_SP112SU:
                raise ValueError("Ricoh SP 112SU is a 3-in-1 flatbed model and does not have an ADF.")
            if not status['adf_has_paper']:
                raise RuntimeError("ADF selected but no paper detected in feeder tray.")

        # Step 2: Pre-configuration carriage homing (Command 3, 0x09)
        print(f"[{self.model_name}] Step 2: Carriage homing before configuration...")
        self.cancel_scan(verbose=True)

        # Step 3: Configure scan geometry (Command 2, 0x0C, len 17, Write-Only)
        print(f"[{self.model_name}] Step 3: Configuring geometry: requested {dpi} DPI (hardware {hw_dpi} DPI), mode={mode}, source={source}, requested_dims={width_px}x{height_px} (Write-Only)...")
        self.configure_scan(dpi=dpi, mode=mode, source=source, width_px=width_px, height_px=height_px)

        # Step 4: Query optical capabilities & scan window confirmation (Command 4, 0x0B, len 8)
        print(f"[{self.model_name}] Step 4: Querying optical capabilities & scan geometry (Command 4)...")
        caps = self.get_capabilities()
        print(f"Capabilities: Logic {caps['logic_width']}x{caps['logic_height']}, Actual {caps['actual_width']}x{caps['actual_height']}")

        # Synchronize dimensions with committed logic dimensions from hardware
        final_w = caps['logic_width'] if caps['logic_width'] > 0 else hw_w
        final_h = caps['logic_height'] if caps['logic_height'] > 0 else hw_h
        adf_600_doubling = (source == 'adf' and hw_dpi >= 600)
        out_h = final_h * 2 if adf_600_doubling else final_h

        # Step 5: Pre-calibration carriage homing (Command 3, 0x09)
        print(f"[{self.model_name}] Step 5: Carriage homing before lamp calibration...")
        self.cancel_scan(verbose=True)

        # Hardware stream dimensions
        hw_bytes_per_line = final_w * 3 if mode == 'color' else final_w
        expected_hw_bytes = hw_bytes_per_line * final_h

        # Calculate scanline-aligned chunk size
        lines_per_chunk = max(1, 65535 // hw_bytes_per_line) if hw_bytes_per_line <= 65535 else 1
        if hw_dpi >= 600:
            lines_per_chunk = max(1, lines_per_chunk // 2)
        chunk_size = lines_per_chunk * hw_bytes_per_line

        # Step 6: Lamp calibration & control (Command 5, 0x08, len 4, Write-Only)
        print(f"[{self.model_name}] Step 6: Calibrating lamp (Write-Only, total_bytes={expected_hw_bytes})...")
        self.lamp_control(expected_hw_bytes)

        # Step 7: Streaming raw raster scanlines (Command 6, 0x0E per chunk)
        print(f"[{self.model_name}] Step 7: Streaming raw raster scanlines (expecting {expected_hw_bytes} hardware bytes in {chunk_size}-byte chunks)...")
        raw_hw_bytes = bytearray()
        try:
            chunk_num = 0
            while len(raw_hw_bytes) < expected_hw_bytes:
                to_read = min(chunk_size, expected_hw_bytes - len(raw_hw_bytes))

                # Step 7a: Dispatch CMD_StartScan (0x0E) with chunk_size before each chunk
                self.start_scan(to_read)

                # Stream raster data from dedicated raster endpoint (EP 0x85)
                timeout_ms = 20000 if chunk_num == 0 else 10000
                try:
                    chunk = self._bulk_read(ENDPOINT_RASTER_IN, to_read, timeout_ms=timeout_ms)
                except Exception:
                    # Fallback to EP 0x84 if needed
                    chunk = self._bulk_read(ENDPOINT_RESP_IN, to_read, timeout_ms=5000)
                if not chunk:
                    break
                raw_hw_bytes.extend(chunk)
                chunk_num += 1
                progress = min(100.0, (len(raw_hw_bytes) / expected_hw_bytes) * 100.0)
                print(f"\rProgress: {progress:5.1f}% ({min(len(raw_hw_bytes), expected_hw_bytes)}/{expected_hw_bytes} bytes)", end="", flush=True)

        except Exception as e:
            print(f"\nScan interrupted: {e}")
            self.close_session()
            raise

        print(f"\nAcquisition complete! Total received: {len(raw_hw_bytes)} bytes.")

        # Step 8: Close active scan session & park carriage
        print(f"[{self.model_name}] Step 8: Closing session (0x0A) and parking carriage (0x09)...")
        self.close_session()

        # Step 9: Post-processing (planar de-interleaving, 3:2 / 3:1 downsampling, 600 DPI line doubling)
        num_hw_lines = len(raw_hw_bytes) // hw_bytes_per_line
        target_w = width_px
        target_h = height_px * 2 if adf_600_doubling else height_px
        print(f"[{self.model_name}] Processing raster data ({target_w}x{target_h})...")
        output_raster = bytearray()

        for l in range(num_hw_lines):
            # Downscaling vertical line selection from 300 DPI hardware raster:
            # - 200 DPI (3:2 downscale): keep 2 out of every 3 lines (drop l % 3 == 2)
            # - 100 DPI (3:1 downscale): keep 1 out of every 3 lines (keep l % 3 == 0)
            if downscale_200 and (l % 3 == 2):
                continue
            if downscale_100 and (l % 3 != 0):
                continue

            hw_line = raw_hw_bytes[l * hw_bytes_per_line : (l + 1) * hw_bytes_per_line]
            if mode == 'color':
                r_plane = hw_line[0 : final_w]
                g_plane = hw_line[final_w : 2 * final_w]
                b_plane = hw_line[2 * final_w : 3 * final_w]
                line_out = bytearray(target_w * 3)
                if downscale_200:
                    for p in range(target_w):
                        sp = (p // 2) * 3 + (p % 2)
                        line_out[p * 3 + 0] = r_plane[sp]
                        line_out[p * 3 + 1] = g_plane[sp]
                        line_out[p * 3 + 2] = b_plane[sp]
                elif downscale_100:
                    for p in range(target_w):
                        sp = p * 3
                        line_out[p * 3 + 0] = r_plane[sp]
                        line_out[p * 3 + 1] = g_plane[sp]
                        line_out[p * 3 + 2] = b_plane[sp]
                else:
                    for p in range(target_w):
                        line_out[p * 3 + 0] = r_plane[p]
                        line_out[p * 3 + 1] = g_plane[p]
                        line_out[p * 3 + 2] = b_plane[p]
            elif mode == 'gray':
                if downscale_200:
                    line_out = bytearray(hw_line[(p // 2) * 3 + (p % 2)] for p in range(target_w))
                elif downscale_100:
                    line_out = bytearray(hw_line[p * 3] for p in range(target_w))
                else:
                    line_out = bytearray(hw_line[:target_w])
            else:
                # 1-bit lineart (packed 1-bit PBM format: 1=black, 0=white)
                bpl = (target_w + 7) // 8
                line_out = bytearray(bpl)
                for p in range(target_w):
                    if downscale_200:
                        val = hw_line[(p // 2) * 3 + (p % 2)]
                    elif downscale_100:
                        val = hw_line[p * 3]
                    else:
                        val = hw_line[p]
                    if val < 128:
                        line_out[p // 8] |= (1 << (7 - (p % 8)))

            output_raster.extend(line_out)
            if adf_600_doubling:
                output_raster.extend(line_out)

        # Write PNM file
        if mode == 'color':
            magic = 'P6'
            header_str = f"{magic}\n{target_w} {target_h}\n255\n"
        elif mode == 'gray':
            magic = 'P5'
            header_str = f"{magic}\n{target_w} {target_h}\n255\n"
        else:
            magic = 'P4'
            header_str = f"{magic}\n{target_w} {target_h}\n"

        with open(output_path, 'wb') as f:
            f.write(header_str.encode('ascii'))
            f.write(output_raster)

        print(f"Saved scanned image to: {output_path}")


# ---------------------------------------------------------------------------
# CLI Entrypoint
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Ricoh SP 112SF / SP 112SU Scanner CLI Driver")
    parser.add_argument("--status", action="store_true", help="Query and display scanner status")
    parser.add_argument("--cancel", "--home", dest="cancel", action="store_true", help="Send CMD_CancelScan (0x09) with 1-byte ACK to return carriage home and clear state")
    parser.add_argument("--reset", action="store_true", help="Perform software endpoint stall clearance and carriage homing")
    parser.add_argument("--scan", type=str, metavar="OUTPUT.pnm", help="Perform a scan and save to PNM file")
    parser.add_argument("--dpi", type=int, choices=[100, 200, 300, 600], default=300, help="Resolution in DPI")
    parser.add_argument("--mode", choices=["color", "gray", "mono"], default="color", help="Color mode")
    parser.add_argument("--source", choices=["flatbed", "adf"], default="flatbed", help="Scan source")
    parser.add_argument("-s", "--paper-size", "--size", dest="paper_size", choices=list(PAPER_SIZES.keys()), help="Predefined paper size (a4, a5, a6, b5, b6, legal, letter)")
    parser.add_argument("--width", type=int, default=0, help="Scan width in pixels (computed automatically if --paper-size specified)")
    parser.add_argument("--height", type=int, default=0, help="Scan height in pixels (computed automatically if --paper-size specified)")

    args = parser.parse_args()

    # Determine width and height
    dpi = args.dpi
    source = args.source
    if args.paper_size:
        w_mm, h_mm = PAPER_SIZES[args.paper_size.lower()]
        if source == "flatbed" and h_mm > 297.0:
            h_mm = 297.0
        width_px = int((w_mm / 25.4) * dpi)
        height_px = int((h_mm / 25.4) * dpi)
    else:
        width_px = args.width if args.width > 0 else int((215.9 / 25.4) * dpi)
        height_px = args.height if args.height > 0 else int((297.0 / 25.4) * dpi)

    try:
        with RicohScannerUSB() as scanner:
            print(f"Connected to {scanner.model_name} (PID 0x{scanner.pid:04x})")

            if args.cancel:
                ack = scanner.cancel_scan(verbose=True)
                time.sleep(0.5)
                st = scanner.query_status()
                print("Scanner Status after CMD_CancelScan:")
                print(f"  Ready:             {st['ready']}")
                print(f"  ADF Paper Present: {st['adf_has_paper']}")
                print(f"  Cover Open:        {st['cover_open']}")
                print(f"  Paper Jam:         {st['paper_jam']}")
                print(f"  Home Position:     {st['home_position']}")
                print(f"  Raw Bytes:         {[hex(b) for b in st['raw']]}")
                return

            if args.reset:
                scanner.reset_device()
                time.sleep(0.5)
                st = scanner.query_status()
                print("Scanner Status after reset:")
                print(f"  Ready:             {st['ready']}")
                print(f"  ADF Paper Present: {st['adf_has_paper']}")
                print(f"  Cover Open:        {st['cover_open']}")
                print(f"  Paper Jam:         {st['paper_jam']}")
                print(f"  Home Position:     {st['home_position']}")
                print(f"  Raw Bytes:         {[hex(b) for b in st['raw']]}")
                return

            if args.status or (not args.scan and not args.reset and not args.cancel):
                st = scanner.query_status()
                print("Scanner Status:")
                print(f"  Ready:             {st['ready']}")
                print(f"  ADF Paper Present: {st['adf_has_paper']}")
                print(f"  Cover Open:        {st['cover_open']}")
                print(f"  Paper Jam:         {st['paper_jam']}")
                print(f"  Home Position:     {st['home_position']}")
                print(f"  Raw Bytes:         {[hex(b) for b in st['raw']]}")

            if args.scan:
                scanner.scan_to_pnm(
                    args.scan,
                    dpi=dpi,
                    mode=args.mode,
                    source=source,
                    width_px=width_px,
                    height_px=height_px
                )
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()

