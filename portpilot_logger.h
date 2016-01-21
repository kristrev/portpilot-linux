#ifndef PORTPILOT_LOGGER_H
#define PORTPILOT_LOGGER_H

#define RETVAL_SUCCESS 1
#define RETVAL_FAILURE 0

#define PORTPILOT_VID 0x16d0
#define PORTPILOT_PID 0x08ac

#define MAX_USB_STR_LEN 0xFF

#define USB_MAX_PATH 8 //(bus + port numbers (max. 7))

#define CSV_DESCRIPTION "Dev. serial, VBus in (mV), VBus out (mV), " \
                        "Current (mA), Max current (mA), Energy (mW), " \
                        "Total energy (mWh)"

#include <stdint.h>
#include <sys/queue.h>

struct backend_event_loop;
struct backend_epoll_handle;
struct backend_timeout_handle;
struct libusb_device_handle;
struct libusb_transfer;
struct portpilot_ctx;

struct portpilot_data {
    uint32_t tstamp;
    uint32_t v_in;
    uint32_t v_out;
    uint32_t energy;
    uint32_t total_energy;
    uint16_t current;
    uint16_t max_current;
    uint16_t num_readings;
};

enum {
    READ_STATE_OK = 0,
    READ_STATE_FAILED_START,
    READ_STATE_RUNNING,
};

struct portpilot_dev {
    struct portpilot_ctx *pp_ctx;
    struct libusb_device_handle *handle;
    struct libusb_transfer *transfer;
    struct portpilot_data *agg_data;
    struct uint8_t *read_buf;
    LIST_ENTRY(portpilot_dev) next_dev;
    uint8_t serial_number[MAX_USB_STR_LEN+1];
    uint16_t max_packet_size;
    uint8_t input_endpoint;
    uint8_t path_len;
    uint8_t read_state;
    uint8_t intf_num;
    uint32_t num_pkts;
    uint8_t path[USB_MAX_PATH];
};

struct portpilot_ctx {
    struct backend_event_loop *event_loop;
    struct backend_epoll_handle *libusb_handle;
    struct backend_timeout_handle *itr_timeout_handle;
    struct backend_timeout_handle *output_timeout_handle;
    LIST_HEAD(dev_list, portpilot_dev) dev_head;
    const char *desired_serial;
    FILE *output_file;
    uint32_t pkts_to_read;
    uint8_t output_interval;
    uint8_t num_done_read;
    uint8_t dev_list_len;
    uint8_t num_itr_req;
    uint8_t verbose;
    uint8_t csv_output;
    uint8_t num_cancel;
    uint8_t num_cancelled;
};

struct portpilot_pkt {
    uint8_t __pad1;
    //sec since boot of device
    uint32_t tstamp;
    //V
    int16_t v_in;
    int16_t v_out;
    //mA
    int16_t current;
    //mA
    int16_t max_current;
    //mWs
    int32_t total_energy;
    uint16_t status_flags;
    uint16_t __pad2;
    uint16_t __pad3;
    //mW
    int16_t energy;
}__attribute((packed))__;

//Functions for updating the reference counter and starting/stopping the
//iteration callback (we only want to run it on every iteration when device
//reading has failed)
void portpilot_logger_start_itr_cb(struct portpilot_ctx *pp_ctx);
void portpilot_logger_stop_itr_cb(struct portpilot_ctx *pp_ctx);

#endif
