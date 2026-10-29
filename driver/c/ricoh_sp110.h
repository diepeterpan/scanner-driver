#ifndef RICOH_SP110_H
#define RICOH_SP110_H

#include <stdint.h>
#include <sane/sane.h>
#include <sane/saneopts.h>
#include <libusb-1.0/libusb.h>

#ifndef SANE_I18N
#define SANE_I18N(text) text
#endif

#define RICOH_VID            0x05CA
#define PID_SP112SU          0x0448   /* 3-in-1 flatbed */
#define PID_SP112SF          0x0449   /* 4-in-1 flatbed + ADF */

#define INTERFACE_SCANNER         0
#define ENDPOINT_BULK_OUT_DEFAULT 0x03   /* EP 3 OUT: Command writes (64-byte framing packets) */
#define ENDPOINT_RESP_IN_DEFAULT  0x85   /* EP 5 IN (Usbscan pipe \2): Command responses & status */
#define ENDPOINT_RASTER_IN_DEFAULT 0x85  /* EP 5 IN (Usbscan pipe \2): High-speed raster image streaming */
#define ENDPOINT_BULK_IN_DEFAULT  0x85   /* Primary IN pipe (0x85) */
#define ENDPOINT_BULK_ALT_DEFAULT 0x84   /* Secondary IN pipe (0x84) */

#define CMD_PACKET_LEN       64
#define CMD_PREAMBLE         0x03

/* Command Subtypes */
#define CMD_SUBTYPE_STATUS    0x0D
#define CMD_SUBTYPE_GEOMETRY  0x0C
#define CMD_SUBTYPE_CANCEL    0x09
#define CMD_SUBTYPE_CAPS      0x0B
#define CMD_SUBTYPE_LAMP      0x08
#define CMD_SUBTYPE_START     0x0E
#define CMD_SUBTYPE_END_SCAN  0x0A
#define CMD_SUBTYPE_WINDOW    0x0A   /* Legacy alias */

/* Expected Response Lengths */
#define CMD_RESP_LEN_STATUS   11
#define CMD_RESP_LEN_GEOMETRY 17
#define CMD_RESP_LEN_CANCEL   1
#define CMD_RESP_LEN_CAPS     8
#define CMD_RESP_LEN_LAMP     4
#define CMD_RESP_LEN_START    4
#define CMD_RESP_LEN_END_SCAN 0
#define CMD_RESP_LEN_WINDOW   10

/* Status query response sensor bitmasks */
#define STATUS_BYTE_ADF_UNIT  0   /* Byte 0 & 0x01: ADF Hardware Present / Installed */
#define STATUS_BYTE_ADF_PAPER 1   /* Byte 1 & 0x01: ADF Paper Loaded in Tray */
#define STATUS_BYTE_SENSORS   1
#define STATUS_BYTE_HOME      2

#define SENSOR_ADF_UNIT       0x01
#define SENSOR_ADF_PAPER      0x01
#define SENSOR_COVER_OPEN     0x02
#define SENSOR_PAPER_JAM      0x04

/* Options enumeration */
enum {
    OPT_NUM_OPTS = 0,
    OPT_MODE_GROUP,
    OPT_MODE,
    OPT_RESOLUTION,
    OPT_SOURCE,
    OPT_GEOMETRY_GROUP,
    OPT_PAPER_SIZE,
    OPT_PAGE_SIZE,
    OPT_TL_X,
    OPT_TL_Y,
    OPT_BR_X,
    OPT_BR_Y,
    OPT_ENHANCEMENT_GROUP,
    OPT_BRIGHTNESS,
    OPT_CONTRAST,
    OPT_THRESHOLD,
    NUM_OPTIONS
};

typedef struct Ricoh_Device {
    struct Ricoh_Device *next;
    SANE_Device sane;
    uint16_t pid;
    char *usb_path;
} Ricoh_Device;

typedef struct Ricoh_Scanner {
    struct Ricoh_Scanner *next;
    Ricoh_Device *hw;
    libusb_device_handle *usb_handle;
    uint8_t ep_out;
    uint8_t ep_in;         /* Command response pipe (EP 0x85) */
    uint8_t ep_raster;     /* Dedicated raster streaming pipe (EP 0x85) */

    /* Configuration options */
    SANE_Option_Descriptor opt[NUM_OPTIONS];
    SANE_Range res_range;
    SANE_Range x_range;
    SANE_Range y_range;
    SANE_Range brightness_range;
    SANE_Range contrast_range;
    SANE_Range threshold_range;

    /* Option Values */
    char *val_mode;
    SANE_Int val_resolution;
    char *val_source;
    char *val_paper_size;
    SANE_Fixed val_tl_x;
    SANE_Fixed val_tl_y;
    SANE_Fixed val_br_x;
    SANE_Fixed val_br_y;
    SANE_Int val_brightness;
    SANE_Int val_contrast;
    SANE_Int val_threshold;
    uint8_t lut[256];

    /* Active Scan State */
    SANE_Parameters params;
    SANE_Bool scanning;
    SANE_Bool cancelled;
    int expected_bytes;
    int bytes_transferred;

    /* Hardware stream parameters */
    int hw_pixels_per_line;
    int hw_bytes_per_line;
    int hw_lines;
    size_t hw_total_bytes;
    size_t hw_bytes_received;
    SANE_Bool adf_600_doubling;
    SANE_Bool downscale_75;
    SANE_Bool downscale_100;
    SANE_Bool downscale_150;
    SANE_Bool downscale_200;
    int hw_line_index;

    /* Bulk transfer chunk buffer */
    uint8_t *raw_chunk_buf;
    size_t raw_chunk_alloc;

    /* Processed (de-planarized / binarized / doubled) output buffer */
    uint8_t *out_buf;
    size_t out_buf_alloc;
    size_t out_buf_len;
    size_t out_buf_pos;
} Ricoh_Scanner;

/* Function Declarations - Both prefixed and standard names */
SANE_Status sane_ricoh_sp110_init(SANE_Int *version_code, SANE_Auth_Callback authorize);
void sane_ricoh_sp110_exit(void);
SANE_Status sane_ricoh_sp110_get_devices(const SANE_Device ***device_list, SANE_Bool local_only);
SANE_Status sane_ricoh_sp110_open(SANE_String_Const devicename, SANE_Handle *handle);
void sane_ricoh_sp110_close(SANE_Handle handle);
const SANE_Option_Descriptor *sane_ricoh_sp110_get_option_descriptor(SANE_Handle handle, SANE_Int option);
SANE_Status sane_ricoh_sp110_control_option(SANE_Handle handle, SANE_Int option, SANE_Action action, void *val, SANE_Int *info);
SANE_Status sane_ricoh_sp110_get_parameters(SANE_Handle handle, SANE_Parameters *params);
SANE_Status sane_ricoh_sp110_start(SANE_Handle handle);
SANE_Status sane_ricoh_sp110_read(SANE_Handle handle, SANE_Byte *data, SANE_Int max_length, SANE_Int *length);
void sane_ricoh_sp110_cancel(SANE_Handle handle);
SANE_Status sane_ricoh_sp110_set_io_mode(SANE_Handle handle, SANE_Bool non_blocking);
SANE_Status sane_ricoh_sp110_get_select_fd(SANE_Handle handle, SANE_Int *fd);

#endif /* RICOH_SP110_H */
