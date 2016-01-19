#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#include "portpilot_callbacks.h"
#include "portpilot_logger.h"
#include "backend_event_loop.h"
#include "portpilot_helpers.h"

void portpilot_cb_libusb_fd_add(int fd, short events, void *data)
{
    struct portpilot_ctx *ctx = data;

    backend_event_loop_update(ctx->event_loop,
                              events,
                              EPOLL_CTL_ADD,
                              fd,
                              ctx->libusb_handle);
}

void portpilot_cb_libusb_fd_remove(int fd, void *data)
{
    //Closing a file descriptor causes it to be removed from epoll-set, this is
    //all we have to do here
    close(fd);
}

void portpilot_cb_itr_cb(void *ptr)
{
    struct portpilot_ctx *pp_ctx = ptr;
    struct timeval tv = {0 ,0};

    //Run libusb timers
    libusb_handle_events_timeout_completed(NULL, &tv, NULL);

    //Check any device for flag set
    if (pp_ctx->dev && pp_ctx->dev->read_state == READ_STATE_FAILED_START)
        portpilot_helpers_start_reading_data(pp_ctx->dev);
}

void portpilot_cb_event_cb(void *ptr, int32_t fd, uint32_t events)
{
    struct timeval tv = {0 ,0};
    libusb_handle_events_timeout_completed(NULL, &tv, NULL);
}

static void portpilot_cb_handle_event_left(libusb_device *device,
        struct portpilot_ctx *pp_ctx, uint8_t *path, uint8_t path_len)
{
    struct portpilot_dev *pp_dev;

    if (!pp_ctx->dev)
        return;

    pp_dev = pp_ctx->dev;

    if (pp_dev->path_len != path_len)
        return;

    if (memcmp(pp_dev->path, path, path_len))
        return;

    fprintf(stderr, "Will remove device with serial number %s\n",
            pp_ctx->dev->serial_number);

    pp_ctx->dev = NULL;

    if (pp_dev->transfer)
        libusb_free_transfer(pp_dev->transfer);

    libusb_release_interface(pp_dev->handle, pp_dev->intf_num);
    libusb_close(pp_dev->handle);
    free(pp_dev);
}

static void portpilot_cb_handle_event_added(libusb_device *device,
        struct portpilot_ctx *pp_ctx, uint8_t *dev_path, uint8_t dev_path_len)
{

    uint8_t conf_desc_idx, input_endpoint, intf_num;
    uint16_t max_packet_size;
    int32_t retval = 0, intf_desc_idx;
    struct libusb_device_descriptor desc = {0};
    struct libusb_config_descriptor *conf_desc = NULL;
    const struct libusb_interface_descriptor *intf_desc;

    if (pp_ctx->dev)
        return;

    libusb_get_device_descriptor(device, &desc);

    //Without a serial number, we can't identify device (should not happen for
    //portpilot)
    if (!desc.iSerialNumber) {
        fprintf(stderr, "Device serial number missing\n");
        return;
    }

    libusb_get_active_config_descriptor(device, &conf_desc);

    if (!conf_desc) {
        fprintf(stderr, "Failed to get config descriptor\n");
        return;
    }

    retval = portpilot_helpers_get_hid_idx(conf_desc, &conf_desc_idx,
            &intf_desc_idx);

    if (!retval) {
        fprintf(stderr, "Failed to get index for HID device\n");
        libusb_free_config_descriptor(conf_desc);
        return;
    }

    intf_desc = &(conf_desc->interface[conf_desc_idx].
            altsetting[intf_desc_idx]);
    intf_num = intf_desc->bInterfaceNumber;

    retval = portpilot_helpers_get_input_info(intf_desc, &input_endpoint,
            &max_packet_size);

    if (!retval) {
        fprintf(stderr, "Failed to get input endpoint info\n");
        libusb_free_config_descriptor(conf_desc);
        return;
    }

    libusb_free_config_descriptor(conf_desc);

    portpilot_helpers_create_dev(device, pp_ctx, max_packet_size,
            input_endpoint, intf_num, dev_path, dev_path_len);
}

int portpilot_cb_libusb_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data)
{
    struct portpilot_ctx *pp_ctx = user_data;
    int32_t retval;
    uint8_t dev_path[USB_MAX_PATH];
    uint8_t dev_path_len;

    //Path is used both on add and remove, so read it already here
    dev_path[0] = libusb_get_port_number(device);
    retval = libusb_get_port_numbers(device, dev_path + 1, 7);
    dev_path_len = retval + 1;

    //Decide how to handle this later
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
        portpilot_cb_handle_event_left(device, pp_ctx, dev_path, dev_path_len);
    else
        portpilot_cb_handle_event_added(device, pp_ctx, dev_path, dev_path_len);

    return 0;
}

//USB callback
void portpilot_cb_read_cb(struct libusb_transfer *transfer)
{
    struct portpilot_dev *pp_dev = transfer->user_data;
    struct portpilot_pkt *pp_pkt = (struct portpilot_pkt*) transfer->buffer;
    //uint8_t i;

    switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        break;
    case LIBUSB_TRANSFER_ERROR:
    case LIBUSB_TRANSFER_TIMED_OUT:
        fprintf(stderr, "Previous transfer failed/timed out, retransmit\n");
        libusb_submit_transfer(transfer);
        return;
    case LIBUSB_TRANSFER_CANCELLED:
        fprintf(stderr, "Transfer was cancelled, not doing anything\n");
        return;
    default:
        fprintf(stderr, "Device stopped working, critical error\n");
        return;
    }

    if (!transfer->actual_length) {
        libusb_submit_transfer(transfer);
        return;
    }

#if 0
    printf("RAW: ");
    for (i = 0; i < transfer->actual_length; i++)
        printf("%x:", transfer->buffer[i]);
    printf("\n");
#endif

    //Add to writer
    fprintf(stdout, "%s,%u,%d,%d,%d,%d,%d,%d\n", pp_dev->serial_number,
            pp_pkt->tstamp, pp_pkt->v_in, pp_pkt->v_out, pp_pkt->current,
            pp_pkt->max_current, pp_pkt->total_energy/3600, pp_pkt->power);

    libusb_submit_transfer(transfer);
}

