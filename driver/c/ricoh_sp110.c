/*
 * SANE backend for Ricoh SP 110 Series Scanners (SP 112SF / SP 112SU)
 *
 * Implements SANE Standard v1.0 over libusb-1.0.
 */

#include "ricoh_sp110.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static libusb_context *usb_ctx = NULL;
static Ricoh_Device *first_dev = NULL;
static const SANE_Device **devlist = NULL;

static const SANE_String_Const mode_list[] = {
    SANE_VALUE_SCAN_MODE_COLOR, SANE_VALUE_SCAN_MODE_GRAY,
    SANE_VALUE_SCAN_MODE_LINEART, NULL};

static const SANE_String_Const source_list_sf[] = {"Flatbed", "ADF", NULL};

static const SANE_String_Const source_list_su[] = {"Flatbed", NULL};

static const SANE_Int resolution_list[] = {4, 100, 200, 300, 600};

typedef struct {
    const char *name;
    double width_mm;
    double height_mm;
} PaperSize;

static const PaperSize paper_sizes[] = {
    {"Custom",   0.0,   0.0},
    {"A4",     210.0, 297.0},
    {"A5",     148.0, 210.0},
    {"A6",     105.0, 148.0},
    {"B5",     176.0, 250.0}, /* ISO B5 */
    {"B6",     125.0, 176.0}, /* ISO B6 */
    {"Legal",  215.9, 355.6}, /* 8.5 x 14 in (ADF: 355.6mm, Flatbed clamped to 297mm) */
    {"Letter", 215.9, 279.4}, /* 8.5 x 11 in */
    {"Max",    215.9, 297.0},
    {NULL,       0.0,   0.0}
};

static const SANE_String_Const paper_size_list[] = {
    "Custom", "A4", "A5", "A6", "B5", "B6", "Legal", "Letter", "Max", NULL
};

/* -------------------------------------------------------------------------
 * Low-level USB protocol helpers
 * ------------------------------------------------------------------------- */

static void detect_endpoints(libusb_device *dev, uint8_t *ep_out,
                             uint8_t *ep_in, uint8_t *ep_raster) {
  *ep_out = ENDPOINT_BULK_OUT_DEFAULT;
  *ep_in = ENDPOINT_RESP_IN_DEFAULT;
  *ep_raster = ENDPOINT_RASTER_IN_DEFAULT;

  struct libusb_config_descriptor *conf = NULL;
  if (libusb_get_active_config_descriptor(dev, &conf) == 0 && conf) {
    for (int i = 0; i < conf->bNumInterfaces; i++) {
      const struct libusb_interface *intf = &conf->interface[i];
      for (int a = 0; a < intf->num_altsetting; a++) {
        const struct libusb_interface_descriptor *desc = &intf->altsetting[a];
        if (desc->bInterfaceNumber == INTERFACE_SCANNER) {
          for (int e = 0; e < desc->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &desc->endpoint[e];
            if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
                LIBUSB_TRANSFER_TYPE_BULK) {
              if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
                  LIBUSB_ENDPOINT_OUT) {
                *ep_out = ep->bEndpointAddress;
              } else if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
                         LIBUSB_ENDPOINT_IN) {
                if (ep->bEndpointAddress == ENDPOINT_RASTER_IN_DEFAULT) {
                  *ep_in = ep->bEndpointAddress;
                  *ep_raster = ep->bEndpointAddress;
                }
              }
            }
          }
        }
      }
    }
    libusb_free_config_descriptor(conf);
  }
}

static int send_command_packet(libusb_device_handle *handle, uint8_t ep_out,
                               uint8_t subtype, uint8_t resp_len,
                               const uint8_t *payload, size_t payload_len) {
  uint8_t pkt[CMD_PACKET_LEN];
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = CMD_PREAMBLE;
  pkt[1] = subtype;
  pkt[2] = resp_len;
  /* pkt[3..5] are 0 */

  if (payload && payload_len > 0) {
    size_t copy_len = (payload_len > (CMD_PACKET_LEN - 6))
                          ? (CMD_PACKET_LEN - 6)
                          : payload_len;
    memcpy(&pkt[6], payload, copy_len);
  }

  int transferred = 0;
  int ret = libusb_bulk_transfer(handle, ep_out, pkt, sizeof(pkt), &transferred,
                                 5000);
  return (ret == 0 && transferred == sizeof(pkt)) ? 0 : -1;
}

static int receive_response_packet(libusb_device_handle *handle, uint8_t ep_in,
                                   uint8_t *resp, int expected_len,
                                   int timeout_ms) {
  int transferred = 0;
  int ret = libusb_bulk_transfer(handle, ep_in, resp, expected_len,
                                 &transferred, timeout_ms);
  return (ret == 0 && transferred > 0) ? transferred : -1;
}

static int fetch_raster_chunk(Ricoh_Scanner *s, uint8_t *buf, size_t to_fetch,
                              int timeout_ms) {
  uint8_t start_payload[4];
  start_payload[0] = (uint8_t)(to_fetch & 0xFF);
  start_payload[1] = (uint8_t)((to_fetch >> 8) & 0xFF);
  start_payload[2] = (uint8_t)((to_fetch >> 16) & 0xFF);
  start_payload[3] = (uint8_t)((to_fetch >> 24) & 0xFF);
  if (send_command_packet(s->usb_handle, s->ep_out, CMD_SUBTYPE_START,
                          CMD_RESP_LEN_START, start_payload,
                          sizeof(start_payload)) != 0) {
    return -1;
  }

  size_t total_read = 0;
  while (total_read < to_fetch) {
    int transferred = 0;
    int ret = libusb_bulk_transfer(
        s->usb_handle, s->ep_raster, buf + total_read,
        (int)(to_fetch - total_read), &transferred,
        (total_read == 0) ? timeout_ms : 5000);
    if (ret != 0 || transferred <= 0) {
      if (total_read == 0) {
        fprintf(stderr,
                "[ricoh_sp110] raster read failed: libusb error %d (%s), "
                "transferred %d bytes\n",
                ret, libusb_error_name(ret), transferred);
        return -1;
      }
      break;
    }
    total_read += (size_t)transferred;
  }
  return (int)total_read;
}

static SANE_Status query_scanner_status(Ricoh_Scanner *s, int *has_paper,
                                        int *cover_open) {
  uint8_t resp[CMD_RESP_LEN_STATUS];
  memset(resp, 0, sizeof(resp));
  if (send_command_packet(s->usb_handle, s->ep_out, CMD_SUBTYPE_STATUS,
                          CMD_RESP_LEN_STATUS, NULL, 0) != 0) {
    return SANE_STATUS_IO_ERROR;
  }

  int rlen = receive_response_packet(s->usb_handle, s->ep_in, resp,
                                     sizeof(resp), 5000);
  if (rlen < 1) {
    return SANE_STATUS_IO_ERROR;
  }

  if (has_paper) {
    *has_paper = (resp[STATUS_BYTE_ADF_PAPER] & SENSOR_ADF_PAPER) ? 1 : 0;
  }
  if (cover_open) {
    *cover_open = (resp[STATUS_BYTE_SENSORS] & SENSOR_COVER_OPEN) ? 1 : 0;
  }

  return SANE_STATUS_GOOD;
}

static void close_scan_session(Ricoh_Scanner *s, SANE_Bool abort_scan) {
  if (!s || !s->usb_handle) {
    return;
  }
  if (abort_scan) {
    /* Aborted mid-scan: explicitly send EndScan (Command 7, 0x0A): Write-Only */
    send_command_packet(s->usb_handle, s->ep_out, CMD_SUBTYPE_END_SCAN,
                        CMD_RESP_LEN_END_SCAN, NULL, 0);
  } else {
    /* Before closing a normal scan session, ensure all hardware bytes declared
     * in CMD_LampControl (0x08) have been drained from the scanner's raster
     * pipeline (matching Windows WIA/TWAIN drivers where DAT_1800490d8 == 0). */
    if (s->hw_bytes_received < s->hw_total_bytes && s->raw_chunk_buf &&
        s->hw_bytes_per_line > 0) {
      int hw_bpl = s->hw_bytes_per_line;
      int lines_per_chunk = (hw_bpl <= 65535) ? (65535 / hw_bpl) : 1;
      if (lines_per_chunk < 1) {
        lines_per_chunk = 1;
      }
      size_t max_chunk = (size_t)(lines_per_chunk * hw_bpl);
      if (max_chunk > s->raw_chunk_alloc) {
        max_chunk = s->raw_chunk_alloc;
      }
      while (s->hw_bytes_received < s->hw_total_bytes) {
        size_t rem = s->hw_total_bytes - s->hw_bytes_received;
        size_t drain_len = (rem > max_chunk) ? max_chunk : rem;
        int got = fetch_raster_chunk(s, s->raw_chunk_buf, drain_len, 5000);
        if (got <= 0) {
          break;
        }
        s->hw_bytes_received += (size_t)got;
      }
    }

    /* Normal scan completion: poll carriage status (Command 3, 0x09) first,
     * matching original Windows WIA/TWAIN drivers (FUN_10031180 / scan_session_sequence).
     * Only send EndScan (0x0A) if the status byte is non-zero. */
    uint8_t ack = 0xFF;
    int rlen = -1;
    if (send_command_packet(s->usb_handle, s->ep_out, CMD_SUBTYPE_CANCEL,
                            CMD_RESP_LEN_CANCEL, NULL, 0) == 0) {
      rlen = receive_response_packet(s->usb_handle, s->ep_in, &ack, sizeof(ack), 1000);
    }
    if (rlen < 1 || ack != 0) {
      send_command_packet(s->usb_handle, s->ep_out, CMD_SUBTYPE_END_SCAN,
                          CMD_RESP_LEN_END_SCAN, NULL, 0);
    }
  }
}

/* -------------------------------------------------------------------------
 * SANE API Implementation (sane_ricoh_sp110_*)
 * ------------------------------------------------------------------------- */

SANE_Status sane_ricoh_sp110_init(SANE_Int *version_code,
                                  SANE_Auth_Callback authorize) {
  (void)authorize;
  if (version_code) {
    *version_code = SANE_VERSION_CODE(1, 0, 0);
  }

  if (!usb_ctx) {
    if (libusb_init(&usb_ctx) != 0) {
      return SANE_STATUS_IO_ERROR;
    }
  }
  return SANE_STATUS_GOOD;
}

void sane_ricoh_sp110_exit(void) {
  Ricoh_Device *curr = first_dev;
  while (curr) {
    Ricoh_Device *next = curr->next;
    free((void *)curr->sane.name);
    free((void *)curr->sane.model);
    free(curr->usb_path);
    free(curr);
    curr = next;
  }
  first_dev = NULL;

  if (devlist) {
    free((void *)devlist);
    devlist = NULL;
  }

  if (usb_ctx) {
    libusb_exit(usb_ctx);
    usb_ctx = NULL;
  }
}

SANE_Status sane_ricoh_sp110_get_devices(const SANE_Device ***device_list,
                                         SANE_Bool local_only) {
  (void)local_only;

  if (!usb_ctx) {
    if (libusb_init(&usb_ctx) != 0) {
      return SANE_STATUS_IO_ERROR;
    }
  }

  /* Free any previous enumeration list to avoid duplicates on multiple queries
   */
  Ricoh_Device *curr = first_dev;
  while (curr) {
    Ricoh_Device *next = curr->next;
    free((void *)curr->sane.name);
    free((void *)curr->sane.model);
    free(curr->usb_path);
    free(curr);
    curr = next;
  }
  first_dev = NULL;

  if (devlist) {
    free((void *)devlist);
    devlist = NULL;
  }

  libusb_device **devs;
  ssize_t cnt = libusb_get_device_list(usb_ctx, &devs);
  if (cnt < 0) {
    return SANE_STATUS_IO_ERROR;
  }

  int num_found = 0;
  for (ssize_t i = 0; i < cnt; i++) {
    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(devs[i], &desc) == 0) {
      if (desc.idVendor == RICOH_VID &&
          (desc.idProduct == PID_SP112SF || desc.idProduct == PID_SP112SU)) {
        uint8_t bus = libusb_get_bus_number(devs[i]);
        uint8_t addr = libusb_get_device_address(devs[i]);
        char devname[64];
        snprintf(devname, sizeof(devname), "libusb:%03d:%03d", bus, addr);

        Ricoh_Device *d = calloc(1, sizeof(Ricoh_Device));
        d->sane.name = strdup(devname);
        d->sane.vendor = "Ricoh";
        d->sane.model = (desc.idProduct == PID_SP112SF)
                            ? strdup("SP 112SF (4-in-1 ADF)")
                            : strdup("SP 112SU (3-in-1)");
        d->sane.type = "flatbed scanner";
        d->pid = desc.idProduct;
        d->usb_path = strdup(devname);

        d->next = first_dev;
        first_dev = d;
        num_found++;
      }
    }
  }
  libusb_free_device_list(devs, 1);

  devlist = calloc(num_found + 1, sizeof(SANE_Device *));
  curr = first_dev;
  for (int i = 0; i < num_found && curr; i++) {
    devlist[i] = &curr->sane;
    curr = curr->next;
  }
  devlist[num_found] = NULL;

  if (device_list) {
    *device_list = devlist;
  }
  return SANE_STATUS_GOOD;
}

SANE_Status sane_ricoh_sp110_open(SANE_String_Const devicename,
                                  SANE_Handle *handle) {
  if (!usb_ctx) {
    if (libusb_init(&usb_ctx) != 0) {
      return SANE_STATUS_IO_ERROR;
    }
  }

  /* Auto-populate devices if not yet enumerated (e.g. direct scanimage --device call) */
  if (!first_dev) {
    sane_ricoh_sp110_get_devices(NULL, SANE_FALSE);
  }

  if (!first_dev) {
    return SANE_STATUS_INVAL;
  }

  Ricoh_Device *matched = NULL;
  if (devicename && devicename[0] != '\0') {
    const char *target = devicename;
    /* Strip leading backend prefix(es) if present, e.g. "ricoh_sp110:" */
    while (strncmp(target, "ricoh_sp110:", 12) == 0) {
      target += 12;
    }

    for (Ricoh_Device *d = first_dev; d; d = d->next) {
      if (strcmp(d->sane.name, target) == 0 ||
          strcmp(d->sane.name, devicename) == 0) {
        matched = d;
        break;
      }
    }
  }

  /* Default to first discovered device if no exact match or empty devicename */
  if (!matched) {
    matched = first_dev;
  }

  libusb_device_handle *usb_h = NULL;
  libusb_device *found_raw_dev = NULL;
  libusb_device **devs;
  ssize_t cnt = libusb_get_device_list(usb_ctx, &devs);
  for (ssize_t i = 0; i < cnt; i++) {
    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(devs[i], &desc) == 0) {
      if (desc.idVendor == RICOH_VID && desc.idProduct == matched->pid) {
        uint8_t bus = libusb_get_bus_number(devs[i]);
        uint8_t addr = libusb_get_device_address(devs[i]);
        char devname[64];
        snprintf(devname, sizeof(devname), "libusb:%03d:%03d", bus, addr);
        if (strcmp(devname, matched->sane.name) == 0 || cnt <= 2) {
          if (libusb_open(devs[i], &usb_h) == 0) {
            found_raw_dev = devs[i];
            break;
          }
        }
      }
    }
  }

  if (!usb_h) {
    libusb_free_device_list(devs, 1);
    return SANE_STATUS_IO_ERROR;
  }

  uint8_t ep_out = ENDPOINT_BULK_OUT_DEFAULT;
  uint8_t ep_in = ENDPOINT_RESP_IN_DEFAULT;
  uint8_t ep_raster = ENDPOINT_RASTER_IN_DEFAULT;
  if (found_raw_dev) {
    detect_endpoints(found_raw_dev, &ep_out, &ep_in, &ep_raster);
  }
  libusb_free_device_list(devs, 1);

  if (libusb_kernel_driver_active(usb_h, INTERFACE_SCANNER) == 1) {
    libusb_detach_kernel_driver(usb_h, INTERFACE_SCANNER);
  }
  if (libusb_claim_interface(usb_h, INTERFACE_SCANNER) != 0) {
    libusb_close(usb_h);
    return SANE_STATUS_DEVICE_BUSY;
  }

  /* Reset interface alternate setting and clear halt to recover from any wedged state */
  libusb_set_interface_alt_setting(usb_h, INTERFACE_SCANNER, 0);
  libusb_clear_halt(usb_h, ep_out);
  libusb_clear_halt(usb_h, ep_in);
  libusb_clear_halt(usb_h, ep_raster);

  /* Drain any residual bulk IN bytes */
  uint8_t drain_buf[4096];
  int drain_len = 0;
  libusb_bulk_transfer(usb_h, ep_in, drain_buf, sizeof(drain_buf), &drain_len, 20);
  libusb_bulk_transfer(usb_h, ep_raster, drain_buf, sizeof(drain_buf), &drain_len, 20);

  Ricoh_Scanner *s = calloc(1, sizeof(Ricoh_Scanner));
  s->hw = matched;
  s->usb_handle = usb_h;
  s->ep_out = ep_out;
  s->ep_in = ep_in;
  s->ep_raster = ep_raster;

  /* Initialize options */
  s->val_mode = strdup(SANE_VALUE_SCAN_MODE_COLOR);
  s->val_resolution = 300;
  s->val_source = strdup("Flatbed");
  s->val_paper_size = strdup("Max");

  /* Default Letter/A4 geometry in mm (fixed-point SANE format) */
  s->val_tl_x = SANE_FIX(0.0);
  s->val_tl_y = SANE_FIX(0.0);
  s->val_br_x = SANE_FIX(215.9);
  s->val_br_y = SANE_FIX(297.0);

  /* Geometry ranges */
  s->x_range.min = SANE_FIX(0.0);
  s->x_range.max = SANE_FIX(215.9);
  s->x_range.quant = 0;

  s->y_range.min = SANE_FIX(0.0);
  s->y_range.max = SANE_FIX(355.6); /* Up to Legal for ADF */
  s->y_range.quant = 0;

  *handle = (SANE_Handle)s;
  return SANE_STATUS_GOOD;
}

void sane_ricoh_sp110_close(SANE_Handle handle) {
  Ricoh_Scanner *s = (Ricoh_Scanner *)handle;
  if (!s)
    return;

  if (s->scanning) {
    sane_ricoh_sp110_cancel(handle);
  }

  if (s->usb_handle) {
    libusb_release_interface(s->usb_handle, INTERFACE_SCANNER);
    libusb_close(s->usb_handle);
    s->usb_handle = NULL;
  }

  if (s->val_mode)
    free(s->val_mode);
  if (s->val_source)
    free(s->val_source);
  if (s->val_paper_size)
    free(s->val_paper_size);
  if (s->raw_chunk_buf)
    free(s->raw_chunk_buf);
  if (s->out_buf)
    free(s->out_buf);

  free(s);
}

const SANE_Option_Descriptor *
sane_ricoh_sp110_get_option_descriptor(SANE_Handle handle, SANE_Int option) {
  Ricoh_Scanner *s = (Ricoh_Scanner *)handle;
  if (!s || option < 0 || option >= NUM_OPTIONS) {
    return NULL;
  }

  SANE_Option_Descriptor *opt = &s->opt[option];
  memset(opt, 0, sizeof(*opt));

  switch (option) {
  case OPT_NUM_OPTS:
    opt->name = SANE_NAME_NUM_OPTIONS;
    opt->title = SANE_TITLE_NUM_OPTIONS;
    opt->desc = SANE_DESC_NUM_OPTIONS;
    opt->type = SANE_TYPE_INT;
    opt->unit = SANE_UNIT_NONE;
    opt->size = sizeof(SANE_Int);
    opt->cap = SANE_CAP_SOFT_DETECT;
    opt->constraint_type = SANE_CONSTRAINT_NONE;
    break;

  case OPT_MODE_GROUP:
    opt->name = "";
    opt->title = "Scan Mode Options";
    opt->desc = "Scan mode and resolution settings";
    opt->type = SANE_TYPE_GROUP;
    opt->unit = SANE_UNIT_NONE;
    opt->size = 0;
    opt->cap = 0;
    opt->constraint_type = SANE_CONSTRAINT_NONE;
    break;

  case OPT_MODE:
    opt->name = SANE_NAME_SCAN_MODE;
    opt->title = SANE_TITLE_SCAN_MODE;
    opt->desc = SANE_DESC_SCAN_MODE;
    opt->type = SANE_TYPE_STRING;
    opt->unit = SANE_UNIT_NONE;
    opt->size = 32;
    opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    opt->constraint_type = SANE_CONSTRAINT_STRING_LIST;
    opt->constraint.string_list = mode_list;
    break;

  case OPT_RESOLUTION:
    opt->name = SANE_NAME_SCAN_RESOLUTION;
    opt->title = SANE_TITLE_SCAN_RESOLUTION;
    opt->desc = SANE_DESC_SCAN_RESOLUTION;
    opt->type = SANE_TYPE_INT;
    opt->unit = SANE_UNIT_DPI;
    opt->size = sizeof(SANE_Int);
    opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    opt->constraint_type = SANE_CONSTRAINT_WORD_LIST;
    opt->constraint.word_list = resolution_list;
    break;

  case OPT_SOURCE:
    opt->name = SANE_NAME_SCAN_SOURCE;
    opt->title = SANE_TITLE_SCAN_SOURCE;
    opt->desc = SANE_DESC_SCAN_SOURCE;
    opt->type = SANE_TYPE_STRING;
    opt->unit = SANE_UNIT_NONE;
    opt->size = 32;
    opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    opt->constraint_type = SANE_CONSTRAINT_STRING_LIST;
    opt->constraint.string_list =
        (s->hw && s->hw->pid == PID_SP112SF) ? source_list_sf : source_list_su;
    break;

  case OPT_GEOMETRY_GROUP:
    opt->name = "";
    opt->title = "Geometry Options";
    opt->desc = "Scan area geometry settings";
    opt->type = SANE_TYPE_GROUP;
    opt->unit = SANE_UNIT_NONE;
    opt->size = 0;
    opt->cap = 0;
    opt->constraint_type = SANE_CONSTRAINT_NONE;
    break;

  case OPT_PAPER_SIZE:
    opt->name = "paper-size";
    opt->title = "Paper size";
    opt->desc = "Predefined paper format (A4, A5, A6, B5, B6, Legal, Letter)";
    opt->type = SANE_TYPE_STRING;
    opt->unit = SANE_UNIT_NONE;
    opt->size = 32;
    opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    opt->constraint_type = SANE_CONSTRAINT_STRING_LIST;
    opt->constraint.string_list = paper_size_list;
    break;

  case OPT_PAGE_SIZE:
    opt->name = "page-size";
    opt->title = "Page size";
    opt->desc = "Predefined page format alias (A4, A5, A6, B5, B6, Legal, Letter)";
    opt->type = SANE_TYPE_STRING;
    opt->unit = SANE_UNIT_NONE;
    opt->size = 32;
    opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    opt->constraint_type = SANE_CONSTRAINT_STRING_LIST;
    opt->constraint.string_list = paper_size_list;
    break;

  case OPT_TL_X:
  case OPT_TL_Y:
  case OPT_BR_X:
  case OPT_BR_Y:
    opt->type = SANE_TYPE_FIXED;
    opt->unit = SANE_UNIT_MM;
    opt->size = sizeof(SANE_Fixed);
    opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    opt->constraint_type = SANE_CONSTRAINT_RANGE;
    if (option == OPT_TL_X) {
      opt->name = SANE_NAME_SCAN_TL_X;
      opt->title = SANE_TITLE_SCAN_TL_X;
      opt->desc = SANE_DESC_SCAN_TL_X;
      opt->constraint.range = &s->x_range;
    }
    if (option == OPT_TL_Y) {
      opt->name = SANE_NAME_SCAN_TL_Y;
      opt->title = SANE_TITLE_SCAN_TL_Y;
      opt->desc = SANE_DESC_SCAN_TL_Y;
      opt->constraint.range = &s->y_range;
    }
    if (option == OPT_BR_X) {
      opt->name = SANE_NAME_SCAN_BR_X;
      opt->title = SANE_TITLE_SCAN_BR_X;
      opt->desc = SANE_DESC_SCAN_BR_X;
      opt->constraint.range = &s->x_range;
    }
    if (option == OPT_BR_Y) {
      opt->name = SANE_NAME_SCAN_BR_Y;
      opt->title = SANE_TITLE_SCAN_BR_Y;
      opt->desc = SANE_DESC_SCAN_BR_Y;
      opt->constraint.range = &s->y_range;
    }
    break;
  }
  return opt;
}

static void apply_paper_size(Ricoh_Scanner *s, const char *size_name, SANE_Int *info) {
  if (!s || !size_name) return;

  for (int i = 0; paper_sizes[i].name != NULL; i++) {
    if (strcasecmp(paper_sizes[i].name, size_name) == 0) {
      if (strcasecmp(size_name, "Custom") == 0) {
        free(s->val_paper_size);
        s->val_paper_size = strdup("Custom");
        return;
      }

      double w = paper_sizes[i].width_mm;
      double h = paper_sizes[i].height_mm;

      /* Clamp Flatbed height to 297mm if Legal is selected on Flatbed */
      if (strcmp(s->val_source, "Flatbed") == 0 && h > 297.0) {
        h = 297.0;
      }

      s->val_tl_x = SANE_FIX(0.0);
      s->val_tl_y = SANE_FIX(0.0);
      s->val_br_x = SANE_FIX(w);
      s->val_br_y = SANE_FIX(h);

      free(s->val_paper_size);
      s->val_paper_size = strdup(paper_sizes[i].name);

      if (info) {
        *info |= SANE_INFO_RELOAD_PARAMS | SANE_INFO_RELOAD_OPTIONS;
      }
      return;
    }
  }
}

SANE_Status sane_ricoh_sp110_control_option(SANE_Handle handle, SANE_Int option,
                                            SANE_Action action, void *val,
                                            SANE_Int *info) {
  Ricoh_Scanner *s = (Ricoh_Scanner *)handle;
  if (info)
    *info = 0;

  if (action == SANE_ACTION_GET_VALUE) {
    switch (option) {
    case OPT_NUM_OPTS:
      *(SANE_Int *)val = NUM_OPTIONS;
      break;
    case OPT_MODE:
      strcpy(val, s->val_mode);
      break;
    case OPT_RESOLUTION:
      *(SANE_Int *)val = s->val_resolution;
      break;
    case OPT_SOURCE:
      strcpy(val, s->val_source);
      break;
    case OPT_PAPER_SIZE:
    case OPT_PAGE_SIZE:
      strcpy(val, s->val_paper_size ? s->val_paper_size : "Custom");
      break;
    case OPT_TL_X:
      *(SANE_Fixed *)val = s->val_tl_x;
      break;
    case OPT_TL_Y:
      *(SANE_Fixed *)val = s->val_tl_y;
      break;
    case OPT_BR_X:
      *(SANE_Fixed *)val = s->val_br_x;
      break;
    case OPT_BR_Y:
      *(SANE_Fixed *)val = s->val_br_y;
      break;
    default:
      return SANE_STATUS_INVAL;
    }
    return SANE_STATUS_GOOD;
  }

  if (action == SANE_ACTION_SET_VALUE) {
    switch (option) {
    case OPT_MODE:
      free(s->val_mode);
      s->val_mode = strdup((char *)val);
      if (info)
        *info |= SANE_INFO_RELOAD_PARAMS;
      break;
    case OPT_RESOLUTION:
      s->val_resolution = *(SANE_Int *)val;
      if (info)
        *info |= SANE_INFO_RELOAD_PARAMS;
      break;
    case OPT_SOURCE:
      free(s->val_source);
      s->val_source = strdup((char *)val);
      /* If source changed to Flatbed and current height exceeds 297mm, clamp */
      if (strcmp(s->val_source, "Flatbed") == 0 && SANE_UNFIX(s->val_br_y) > 297.0) {
        s->val_br_y = SANE_FIX(297.0);
        if (info)
          *info |= SANE_INFO_RELOAD_PARAMS | SANE_INFO_RELOAD_OPTIONS;
      }
      break;
    case OPT_PAPER_SIZE:
    case OPT_PAGE_SIZE:
      apply_paper_size(s, (const char *)val, info);
      break;
    case OPT_TL_X:
      s->val_tl_x = *(SANE_Fixed *)val;
      free(s->val_paper_size);
      s->val_paper_size = strdup("Custom");
      if (info) *info |= SANE_INFO_RELOAD_PARAMS;
      break;
    case OPT_TL_Y:
      s->val_tl_y = *(SANE_Fixed *)val;
      free(s->val_paper_size);
      s->val_paper_size = strdup("Custom");
      if (info) *info |= SANE_INFO_RELOAD_PARAMS;
      break;
    case OPT_BR_X:
      s->val_br_x = *(SANE_Fixed *)val;
      free(s->val_paper_size);
      s->val_paper_size = strdup("Custom");
      if (info) *info |= SANE_INFO_RELOAD_PARAMS;
      break;
    case OPT_BR_Y:
      s->val_br_y = *(SANE_Fixed *)val;
      /* Clamp Flatbed to 297.0 mm */
      if (strcmp(s->val_source, "Flatbed") == 0 && SANE_UNFIX(s->val_br_y) > 297.0) {
        s->val_br_y = SANE_FIX(297.0);
      }
      free(s->val_paper_size);
      s->val_paper_size = strdup("Custom");
      if (info) *info |= SANE_INFO_RELOAD_PARAMS;
      break;
    default:
      return SANE_STATUS_INVAL;
    }
    return SANE_STATUS_GOOD;
  }

  return SANE_STATUS_INVAL;
}

SANE_Status sane_ricoh_sp110_get_parameters(SANE_Handle handle,
                                            SANE_Parameters *params) {
  Ricoh_Scanner *s = (Ricoh_Scanner *)handle;
  double width_mm = SANE_UNFIX(s->val_br_x - s->val_tl_x);
  double height_mm = SANE_UNFIX(s->val_br_y - s->val_tl_y);

  int width_px = (int)((width_mm / 25.4) * s->val_resolution);
  int height_px = (int)((height_mm / 25.4) * s->val_resolution);

  if (strcmp(s->val_mode, SANE_VALUE_SCAN_MODE_COLOR) == 0) {
    params->format = SANE_FRAME_RGB;
    params->depth = 8;
    params->bytes_per_line = width_px * 3;
  } else if (strcmp(s->val_mode, SANE_VALUE_SCAN_MODE_GRAY) == 0) {
    params->format = SANE_FRAME_GRAY;
    params->depth = 8;
    params->bytes_per_line = width_px;
  } else {
    params->format = SANE_FRAME_GRAY;
    params->depth = 1;
    params->bytes_per_line = (width_px + 7) / 8;
  }

  params->pixels_per_line = width_px;
  params->lines = height_px;
  params->last_frame = SANE_TRUE;

  s->params = *params;
  return SANE_STATUS_GOOD;
}

SANE_Status sane_ricoh_sp110_start(SANE_Handle handle) {
  Ricoh_Scanner *s = (Ricoh_Scanner *)handle;
  sane_ricoh_sp110_get_parameters(handle, &s->params);

  int has_paper = 0, cover_open = 0;
  SANE_Status st = query_scanner_status(s, &has_paper, &cover_open);
  if (st != SANE_STATUS_GOOD) {
    return st;
  }

  if (strcmp(s->val_source, "ADF") == 0 && !has_paper) {
    return SANE_STATUS_NO_DOCS;
  }

  /* Step 2: Pre-configuration carriage homing (Command 3, 0x09) */
  uint8_t pre_ack = 0;
  if (send_command_packet(s->usb_handle, s->ep_out, CMD_SUBTYPE_CANCEL,
                          CMD_RESP_LEN_CANCEL, NULL, 0) == 0) {
    receive_response_packet(s->usb_handle, s->ep_in, &pre_ack, sizeof(pre_ack), 3000);
  }

  s->downscale_100 = (s->val_resolution == 100) ? SANE_TRUE : SANE_FALSE;
  s->downscale_200 = (s->val_resolution == 200) ? SANE_TRUE : SANE_FALSE;
  /* Hardware only supports 300 DPI (0x01) and 600 DPI (0x02) optical stepping.
   * Both 100 DPI and 200 DPI are captured at 300 DPI hardware resolution. */
  int hw_dpi = (s->val_resolution >= 600) ? 600 : 300;
  s->hw_line_index = 0;

  /* Build 17-byte parameter block */
  uint8_t payload[17];
  memset(payload, 0, sizeof(payload));

  /* Resolution code: 100/200/300 DPI -> 0x01, 600 DPI -> 0x02 */
  payload[0] = (hw_dpi >= 600) ? 0x02 : 0x01;

  /* Hardware ScanMode: gray/lineart -> 0x02, color -> 0x03 */
  payload[1] = (strcmp(s->val_mode, SANE_VALUE_SCAN_MODE_COLOR) == 0) ? 0x03 : 0x02;

  /* Default brightness (0x05) and Custom window paper size ID (0xFF),
   * matching original Windows WIA/TWAIN drivers (FUN_180001000 / FUN_10030bb0). */
  payload[2] = 0x05;
  payload[3] = 0xFF;

  /* Geometry coordinates: Scaled to 600 DPI base units per original driver:
   * coord_600 = (coord_dpi * 600) / requested_dpi.
   * For 200 DPI, (px * 600)/200 = px * 3 = 1.5 * (px at 300 DPI). */
  uint16_t w_600 = (uint16_t)((s->params.pixels_per_line * 600) / s->val_resolution);
  uint16_t h_600 = (uint16_t)((s->params.lines * 600) / s->val_resolution);

  /* Calculate expected physical 300/600 DPI hardware pixel and line counts */
  int hw_req_pixels = (w_600 * hw_dpi) / 600;
  int hw_req_lines = (h_600 * hw_dpi) / 600;

  /* Align height to even number for 600 DPI ADF */
  if (hw_dpi >= 600 && strcmp(s->val_source, "ADF") == 0 && (h_600 & 1)) {
    h_600++;
    hw_req_lines++;
  }

  payload[4] = 0;
  payload[5] = 0; /* StartX */
  payload[6] = 0;
  payload[7] = 0; /* StartY */
  payload[8] = (uint8_t)(w_600 & 0xFF);
  payload[9] = (uint8_t)(w_600 >> 8);
  payload[10] = (uint8_t)(h_600 & 0xFF);
  payload[11] = (uint8_t)(h_600 >> 8);
  payload[12] = (strcmp(s->val_source, "ADF") == 0) ? 0x01 : 0x00;

  /* Step 3: Configure Geometry parameters (Command 2, 0x0C): Write-Only */
  if (send_command_packet(s->usb_handle, s->ep_out, CMD_SUBTYPE_GEOMETRY,
                          CMD_RESP_LEN_GEOMETRY, payload, sizeof(payload)) != 0) {
    return SANE_STATUS_IO_ERROR;
  }

  /* Step 4: Optical capabilities & scan window query (Command 4, 0x0B): Reads 8 bytes */
  uint8_t caps[CMD_RESP_LEN_CAPS];
  memset(caps, 0, sizeof(caps));
  s->hw_pixels_per_line = hw_req_pixels;
  s->hw_lines = hw_req_lines;
  s->adf_600_doubling = (strcmp(s->val_source, "ADF") == 0 && hw_dpi >= 600) ? SANE_TRUE : SANE_FALSE;

  if (send_command_packet(s->usb_handle, s->ep_out, CMD_SUBTYPE_CAPS,
                          CMD_RESP_LEN_CAPS, NULL, 0) == 0) {
    int rlen = receive_response_packet(s->usb_handle, s->ep_in, caps, sizeof(caps), 5000);
    if (rlen >= 8) {
      uint16_t logic_w = (uint16_t)(caps[0] | (caps[1] << 8));
      uint16_t logic_h = (uint16_t)(caps[2] | (caps[3] << 8));
      if (logic_w > 0 && logic_h > 0) {
        s->hw_pixels_per_line = logic_w;
        s->hw_lines = logic_h;
      }
    }
  }


  if (strcmp(s->val_mode, SANE_VALUE_SCAN_MODE_COLOR) == 0) {
    s->params.bytes_per_line = s->params.pixels_per_line * 3;
    s->hw_bytes_per_line = s->hw_pixels_per_line * 3;
  } else if (strcmp(s->val_mode, SANE_VALUE_SCAN_MODE_GRAY) == 0) {
    s->params.bytes_per_line = s->params.pixels_per_line;
    s->hw_bytes_per_line = s->hw_pixels_per_line;
  } else {
    /* 1-bit Lineart: SANE output is packed 1-bit, hardware streams 8-bit grayscale */
    s->params.bytes_per_line = (s->params.pixels_per_line + 7) / 8;
    s->hw_bytes_per_line = s->hw_pixels_per_line;
  }

  s->hw_total_bytes = (size_t)s->hw_bytes_per_line * (size_t)s->hw_lines;
  s->expected_bytes = s->params.bytes_per_line * s->params.lines;

  /* Step 5: Lamp calibration & control (Command 5, 0x08): Write-Only
   * Payload: total hardware bytes (4 bytes little-endian) + 0x01 flag
   */
  uint32_t total_hw_bytes = (uint32_t)s->hw_total_bytes;
  uint8_t lamp_payload[5];
  lamp_payload[0] = (uint8_t)(total_hw_bytes & 0xFF);
  lamp_payload[1] = (uint8_t)((total_hw_bytes >> 8) & 0xFF);
  lamp_payload[2] = (uint8_t)((total_hw_bytes >> 16) & 0xFF);
  lamp_payload[3] = (uint8_t)((total_hw_bytes >> 24) & 0xFF);
  lamp_payload[4] = 0x01;
  if (send_command_packet(s->usb_handle, s->ep_out, CMD_SUBTYPE_LAMP,
                          CMD_RESP_LEN_LAMP, lamp_payload, sizeof(lamp_payload)) != 0) {
    return SANE_STATUS_IO_ERROR;
  }

  s->scanning = SANE_TRUE;
  s->cancelled = SANE_FALSE;
  s->bytes_transferred = 0;
  s->hw_bytes_received = 0;
  s->out_buf_len = 0;
  s->out_buf_pos = 0;
  return SANE_STATUS_GOOD;
}

SANE_Status sane_ricoh_sp110_read(SANE_Handle handle, SANE_Byte *data,
                                  SANE_Int max_length, SANE_Int *length) {
  Ricoh_Scanner *s = (Ricoh_Scanner *)handle;
  *length = 0;

  if (!s->scanning) {
    return SANE_STATUS_EOF;
  }
  if (s->cancelled) {
    return SANE_STATUS_CANCELLED;
  }
  if (s->bytes_transferred >= s->expected_bytes) {
    s->scanning = SANE_FALSE;
    close_scan_session(s, SANE_FALSE);
    return SANE_STATUS_EOF;
  }

  /* Deliver cached processed output buffer first if available */
  if (s->out_buf && s->out_buf_pos < s->out_buf_len) {
    size_t avail = s->out_buf_len - s->out_buf_pos;
    size_t remaining = (size_t)(s->expected_bytes - s->bytes_transferred);
    if (avail > remaining) {
      avail = remaining;
    }
    size_t to_copy = (avail > (size_t)max_length) ? (size_t)max_length : avail;
    memcpy(data, s->out_buf + s->out_buf_pos, to_copy);
    s->out_buf_pos += to_copy;
    s->bytes_transferred += (int)to_copy;
    *length = (SANE_Int)to_copy;
    if (s->bytes_transferred >= s->expected_bytes) {
      s->scanning = SANE_FALSE;
      close_scan_session(s, SANE_FALSE);
    }
    return SANE_STATUS_GOOD;
  }

  /* Check if all physical data from hardware has already been read */
  if (s->hw_bytes_received >= s->hw_total_bytes) {
    s->scanning = SANE_FALSE;
    close_scan_session(s, SANE_FALSE);
    return SANE_STATUS_EOF;
  }

  /* Buffer is empty: calculate line-aligned chunk size */
  int hw_bpl = s->hw_bytes_per_line;
  if (hw_bpl <= 0) {
    return SANE_STATUS_INVAL;
  }
  int lines_per_chunk = (hw_bpl <= 65535) ? (65535 / hw_bpl) : 1;
  if (s->val_resolution >= 600 && lines_per_chunk > 1) {
    lines_per_chunk /= 2;
  }
  if (lines_per_chunk < 1) {
    lines_per_chunk = 1;
  }
  size_t chunk_size = (size_t)(lines_per_chunk * hw_bpl);
  if (chunk_size < 512) {
    chunk_size = 512;
  }

  size_t remaining_hw = s->hw_total_bytes - s->hw_bytes_received;
  size_t to_fetch = (chunk_size > remaining_hw) ? remaining_hw : chunk_size;

  /* Allocate generous 128KB buffers
   * to guarantee no USB transfer or line doubling can ever overflow heap bounds */
  size_t min_alloc = 131072;
  if (!s->raw_chunk_buf || s->raw_chunk_alloc < min_alloc) {
    s->raw_chunk_buf = realloc(s->raw_chunk_buf, min_alloc);
    if (!s->raw_chunk_buf) {
      return SANE_STATUS_NO_MEM;
    }
    s->raw_chunk_alloc = min_alloc;
  }

  if (!s->out_buf || s->out_buf_alloc < min_alloc) {
    s->out_buf = realloc(s->out_buf, min_alloc);
    if (!s->out_buf) {
      return SANE_STATUS_NO_MEM;
    }
    s->out_buf_alloc = min_alloc;
  }
  s->out_buf_len = 0;
  s->out_buf_pos = 0;

  /* Step 7: Dispatch CMD_StartScan (Command 6, 0x0E) and read full requested chunk */
  int timeout_ms = (s->hw_bytes_received == 0) ? 20000 : 10000;
  int transferred = fetch_raster_chunk(s, s->raw_chunk_buf, to_fetch, timeout_ms);

  if (transferred <= 0) {
    if (s->bytes_transferred > 0) {
      s->scanning = SANE_FALSE;
      close_scan_session(s, SANE_FALSE);
      return SANE_STATUS_EOF;
    }
    s->scanning = SANE_FALSE;
    close_scan_session(s, SANE_TRUE);
    return SANE_STATUS_IO_ERROR;
  }

  /* Process transferred hardware lines into SANE output format */
  int lines_in_chunk = transferred / hw_bpl;
  int out_bpl = s->params.bytes_per_line;
  int active_pixels = s->params.pixels_per_line;
  int hw_pitch = s->hw_pixels_per_line;

  for (int l = 0; l < lines_in_chunk; l++) {
    int current_line_idx = s->hw_line_index++;

    /* Downscaling vertical line selection from 300 DPI hardware stream:
     * - 200 DPI (3:2 downscale): keep 2 out of every 3 lines (drop line % 3 == 2)
     * - 100 DPI (3:1 downscale): keep 1 out of every 3 lines (keep line % 3 == 0)
     */
    if (s->downscale_200 && (current_line_idx % 3 == 2)) {
      continue;
    }
    if (s->downscale_100 && (current_line_idx % 3 != 0)) {
      continue;
    }

    const uint8_t *hw_line = s->raw_chunk_buf + l * hw_bpl;
    uint8_t *dst = s->out_buf + s->out_buf_len;

    if (s->downscale_200) {
      /* 200 DPI (3:2 downscaling): map 3 input pixels to 2 output pixels.
       * For output pixel p:
       * src_p = (p / 2) * 3 + (p % 2) -> maps [0,1, 2,3, 4,5] to [0,1, 3,4, 6,7] */
      if (strcmp(s->val_mode, SANE_VALUE_SCAN_MODE_COLOR) == 0) {
        const uint8_t *r_plane = hw_line;
        const uint8_t *g_plane = hw_line + hw_pitch;
        const uint8_t *b_plane = hw_line + hw_pitch * 2;
        for (int p = 0; p < active_pixels; p++) {
          int src_p = (p / 2) * 3 + (p % 2);
          dst[p * 3 + 0] = r_plane[src_p];
          dst[p * 3 + 1] = g_plane[src_p];
          dst[p * 3 + 2] = b_plane[src_p];
        }
      } else if (strcmp(s->val_mode, SANE_VALUE_SCAN_MODE_GRAY) == 0) {
        for (int p = 0; p < active_pixels; p++) {
          int src_p = (p / 2) * 3 + (p % 2);
          dst[p] = hw_line[src_p];
        }
      } else {
        /* 1-bit Lineart */
        memset(dst, 0, (size_t)out_bpl);
        for (int p = 0; p < active_pixels; p++) {
          int src_p = (p / 2) * 3 + (p % 2);
          if (hw_line[src_p] < 128) {
            dst[p / 8] |= (uint8_t)(1 << (7 - (p % 8)));
          }
        }
      }
    } else if (s->downscale_100) {
      /* 100 DPI (3:1 downscaling): keep every 3rd pixel (0, 3, 6...) */
      if (strcmp(s->val_mode, SANE_VALUE_SCAN_MODE_COLOR) == 0) {
        const uint8_t *r_plane = hw_line;
        const uint8_t *g_plane = hw_line + hw_pitch;
        const uint8_t *b_plane = hw_line + hw_pitch * 2;
        for (int p = 0; p < active_pixels; p++) {
          int src_p = p * 3;
          dst[p * 3 + 0] = r_plane[src_p];
          dst[p * 3 + 1] = g_plane[src_p];
          dst[p * 3 + 2] = b_plane[src_p];
        }
      } else if (strcmp(s->val_mode, SANE_VALUE_SCAN_MODE_GRAY) == 0) {
        for (int p = 0; p < active_pixels; p++) {
          dst[p] = hw_line[p * 3];
        }
      } else {
        /* 1-bit Lineart */
        memset(dst, 0, (size_t)out_bpl);
        for (int p = 0; p < active_pixels; p++) {
          if (hw_line[p * 3] < 128) {
            dst[p / 8] |= (uint8_t)(1 << (7 - (p % 8)));
          }
        }
      }
    } else {
      /* Native 300, 600 DPI output */
      if (strcmp(s->val_mode, SANE_VALUE_SCAN_MODE_COLOR) == 0) {
        /* De-planarize Red, Green, Blue planes into interleaved RGB */
        const uint8_t *r_plane = hw_line;
        const uint8_t *g_plane = hw_line + hw_pitch;
        const uint8_t *b_plane = hw_line + hw_pitch * 2;
        for (int p = 0; p < active_pixels; p++) {
          dst[p * 3 + 0] = r_plane[p];
          dst[p * 3 + 1] = g_plane[p];
          dst[p * 3 + 2] = b_plane[p];
        }
      } else if (strcmp(s->val_mode, SANE_VALUE_SCAN_MODE_GRAY) == 0) {
        memcpy(dst, hw_line, (size_t)out_bpl);
      } else {
        /* 1-bit Lineart: threshold 8-bit grayscale at 128 (0=white, 1=black per SANE standard) */
        memset(dst, 0, (size_t)out_bpl);
        for (int p = 0; p < active_pixels; p++) {
          if (hw_line[p] < 128) {
            dst[p / 8] |= (uint8_t)(1 << (7 - (p % 8)));
          }
        }
      }
    }

    s->out_buf_len += (size_t)out_bpl;

    /* Software line doubling for 600 DPI ADF */
    if (s->adf_600_doubling) {
      memcpy(s->out_buf + s->out_buf_len, dst, (size_t)out_bpl);
      s->out_buf_len += (size_t)out_bpl;
    }
  }


  s->hw_bytes_received += (size_t)transferred;

  size_t avail = s->out_buf_len;
  size_t remaining = (size_t)(s->expected_bytes - s->bytes_transferred);
  if (avail > remaining) {
    avail = remaining;
  }
  size_t to_copy = (avail > (size_t)max_length) ? (size_t)max_length : avail;
  memcpy(data, s->out_buf, to_copy);
  s->out_buf_pos = to_copy;
  s->bytes_transferred += (int)to_copy;
  *length = (SANE_Int)to_copy;

  if (s->bytes_transferred >= s->expected_bytes) {
    s->scanning = SANE_FALSE;
    close_scan_session(s, SANE_FALSE);
  }

  return SANE_STATUS_GOOD;
}

void sane_ricoh_sp110_cancel(SANE_Handle handle) {
  Ricoh_Scanner *s = (Ricoh_Scanner *)handle;
  if (s && s->usb_handle) {
    s->cancelled = SANE_TRUE;
    if (s->scanning) {
      s->scanning = SANE_FALSE;
      close_scan_session(s, SANE_TRUE);
    }
  }
}

SANE_Status sane_ricoh_sp110_set_io_mode(SANE_Handle handle,
                                         SANE_Bool non_blocking) {
  (void)handle;
  return (non_blocking == SANE_FALSE) ? SANE_STATUS_GOOD
                                      : SANE_STATUS_UNSUPPORTED;
}

SANE_Status sane_ricoh_sp110_get_select_fd(SANE_Handle handle, SANE_Int *fd) {
  (void)handle;
  (void)fd;
  return SANE_STATUS_UNSUPPORTED;
}

/* -------------------------------------------------------------------------
 * Standard SANE Symbol Aliases (allows both prefixed and non-prefixed entry)
 * ------------------------------------------------------------------------- */
SANE_Status sane_init(SANE_Int *version_code, SANE_Auth_Callback authorize)
    __attribute__((alias("sane_ricoh_sp110_init")));
void sane_exit(void) __attribute__((alias("sane_ricoh_sp110_exit")));
SANE_Status sane_get_devices(const SANE_Device ***device_list,
                             SANE_Bool local_only)
    __attribute__((alias("sane_ricoh_sp110_get_devices")));
SANE_Status sane_open(SANE_String_Const devicename, SANE_Handle *handle)
    __attribute__((alias("sane_ricoh_sp110_open")));
void sane_close(SANE_Handle handle)
    __attribute__((alias("sane_ricoh_sp110_close")));
const SANE_Option_Descriptor *sane_get_option_descriptor(SANE_Handle handle,
                                                         SANE_Int option)
    __attribute__((alias("sane_ricoh_sp110_get_option_descriptor")));
SANE_Status sane_control_option(SANE_Handle handle, SANE_Int option,
                                SANE_Action action, void *val, SANE_Int *info)
    __attribute__((alias("sane_ricoh_sp110_control_option")));
SANE_Status sane_get_parameters(SANE_Handle handle, SANE_Parameters *params)
    __attribute__((alias("sane_ricoh_sp110_get_parameters")));
SANE_Status sane_start(SANE_Handle handle)
    __attribute__((alias("sane_ricoh_sp110_start")));
SANE_Status sane_read(SANE_Handle handle, SANE_Byte *data, SANE_Int max_length,
                      SANE_Int *length)
    __attribute__((alias("sane_ricoh_sp110_read")));
void sane_cancel(SANE_Handle handle)
    __attribute__((alias("sane_ricoh_sp110_cancel")));
SANE_Status sane_set_io_mode(SANE_Handle handle, SANE_Bool non_blocking)
    __attribute__((alias("sane_ricoh_sp110_set_io_mode")));
SANE_Status sane_get_select_fd(SANE_Handle handle, SANE_Int *fd)
    __attribute__((alias("sane_ricoh_sp110_get_select_fd")));
