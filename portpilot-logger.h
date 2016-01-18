#ifndef PORTPILOT_LOGGER_H
#define PORTPILOT_LOGGER_H

#define RETVAL_SUCCESS 1
#define RETVAL_FAILURE 0

struct backend_event_loop;
struct backend_epoll_handle;
struct libusb_device_handle;
struct libusb_transfer;

struct portpilot_ctx {
    struct backend_event_loop *event_loop;
    struct backend_epoll_handle *libusb_handle;
    struct libusb_device_handle *dev_handle;
    int input_endpoint;
    int max_packet_size;
    struct libusb_transfer *transfer;
};

struct portpilot_pkt {
    uint8_t pad;
    uint32_t tstamp;
    int16_t v_in;
    int16_t v_out;
    int16_t current;
    int16_t max_current;
    int32_t total_energy;
    uint16_t status_flags;
    uint16_t __pad1;
    uint16_t __pad2;
    int16_t power;
}__attribute((packed))__;

#endif
