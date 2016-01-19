#ifndef PORTPILOT_LOGGER_H
#define PORTPILOT_LOGGER_H

#define RETVAL_SUCCESS 1
#define RETVAL_FAILURE 0

#define PORTPILOT_VID 0x16d0
#define PORTPILOT_PID 0x08ac

#define MAX_USB_STR_LEN 0xFF

#define USB_MAX_PATH 8 //(bus + port numbers (max. 7))

struct backend_event_loop;
struct backend_epoll_handle;
struct libusb_device_handle;
struct libusb_transfer;

struct portpilot_dev {
    struct libusb_device_handle *handle;
    struct libusb_transfer *transfer;
    uint8_t serial_number[MAX_USB_STR_LEN+1];
    uint16_t max_packet_size;
    uint8_t input_endpoint;
    uint8_t path_len;
    uint8_t path[USB_MAX_PATH];
};

struct portpilot_ctx {
    struct backend_event_loop *event_loop;
    struct backend_epoll_handle *libusb_handle;
    struct portpilot_dev *dev;
};

struct portpilot_pkt {
    uint8_t __pad1;
    uint32_t tstamp;
    int16_t v_in;
    int16_t v_out;
    int16_t current;
    int16_t max_current;
    int32_t total_energy;
    uint16_t status_flags;
    uint16_t __pad2;
    uint16_t __pad3;
    int16_t power;
}__attribute((packed))__;

#endif
