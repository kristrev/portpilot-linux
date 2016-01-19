#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libusb-1.0/libusb.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "portpilot-logger.h"
#include "backend_event_loop.h"

static void portpilot_libusb_fd_add(int fd, short events, void *data)
{
    struct portpilot_ctx *ctx = data;

    backend_event_loop_update(ctx->event_loop,
                              events,
                              EPOLL_CTL_ADD,
                              fd,
                              ctx->libusb_handle);
}

static void portpilot_libusb_fd_remove(int fd, void *data)
{
    //Closing a file descriptor causes it to be removed from epoll-set
    close(fd);
}

static void portpilot_itr_cb(void *ptr)
{
    struct timeval tv = {0 ,0};

    //Run libusb timers
    libusb_handle_events_timeout_completed(NULL, &tv, NULL);
}

static void portpilot_event_cb(void *ptr, int32_t fd, uint32_t events)
{
    struct timeval tv = {0 ,0};
    libusb_handle_events_timeout_completed(NULL, &tv, NULL);
}

static void portpilot_read_cb(struct libusb_transfer *transfer)
{
    struct portpilot_pkt *ppp = (struct portpilot_pkt*) transfer->buffer;
    uint8_t i;

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        fprintf(stderr, "Failed to read data from device\n");
        return;
    }

    if (!transfer->actual_length) {
        libusb_submit_transfer(transfer);
        return;
    }

    printf("RAW: ");
    for (i = 0; i < transfer->actual_length; i++)
        printf("%x:", transfer->buffer[i]);
    printf("\n");
    printf("tstamp: %us v_in: %dmV v_out: %dmV current: %dmA max. current: %dmA total energy: %dmWh power: %dmW\n",
            ppp->tstamp, ppp->v_in, ppp->v_out, ppp->current, ppp->max_current, ppp->total_energy/3600, ppp->power);

    libusb_submit_transfer(transfer);
}

static void portpilot_read_data(struct portpilot_ctx *ppc)
{
    uint8_t *buf = calloc(ppc->max_packet_size, 1);

    if (!buf) {
        fprintf(stderr, "Could not allocate buffer memory\n");
        exit(EXIT_FAILURE);
    }

    ppc->transfer = libusb_alloc_transfer(0);
    libusb_fill_interrupt_transfer(ppc->transfer, ppc->dev_handle,
            ppc->input_endpoint, buf, ppc->max_packet_size, portpilot_read_cb,
            ppc, 5000);

    libusb_submit_transfer(ppc->transfer);
}

//Get the indexes of the HID interface
static uint8_t portpilot_get_hid_idx(const struct libusb_config_descriptor *conf_desc,
        uint8_t *conf_desc_idx, int32_t *intf_desc_idx)
{
    uint8_t i;
    int32_t j;
    const struct libusb_interface *intf;
    const struct libusb_interface_descriptor *intf_desc;

    for (i = 0; i < conf_desc->bNumInterfaces; i++) {
        intf = &conf_desc->interface[i];

        for (j = 0; j < intf->num_altsetting; j++) {
            intf_desc = &intf->altsetting[j];

            if (intf_desc->bInterfaceClass == LIBUSB_CLASS_HID) {
                *conf_desc_idx = i;
                *intf_desc_idx = j;
                return RETVAL_SUCCESS;
            }
        }
    }
   
    return RETVAL_FAILURE;
}

static uint8_t portpilot_get_input_info(const struct libusb_interface_descriptor *intf_desc,
        uint8_t *input_endpoint, uint16_t *max_packet_size)
{
    uint8_t i, is_interrupt, is_input;
    const struct libusb_endpoint_descriptor *ep;

    for (i = 0; i < intf_desc->bNumEndpoints; i++) {
        ep = &intf_desc->endpoint[i];

        is_interrupt = (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
            LIBUSB_TRANSFER_TYPE_INTERRUPT;
        is_input = (ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
            LIBUSB_ENDPOINT_IN;

        if (is_interrupt && is_input) {
            *input_endpoint = ep->bEndpointAddress;
            *max_packet_size = ep->wMaxPacketSize;
            return RETVAL_SUCCESS;
        }
    }

    return RETVAL_FAILURE;
}

static int portpilot_event_left(libusb_device *device,
        struct portpilot_ctx *pp_ctx)
{
    if (!pp_ctx->dev)
        return 0;

    fprintf(stderr, "Will remove device with serial number %s\n",
            pp_ctx->dev->serial_number);

    //TODO: Will be replaced with a list delete
    libusb_close(pp_ctx->dev->handle);
    free(pp_ctx->dev);
    pp_ctx->dev = NULL;
    return 0;
}

static int portpilot_create_dev(libusb_device *device,
        struct portpilot_ctx *pp_ctx, uint16_t max_packet_size,
        uint8_t input_endpoint, uint8_t intf_num)
{
    int32_t retval;
    struct portpilot_dev *pp_dev = NULL;
    struct libusb_device_descriptor desc = {0};

    //TODO: Split into new function?
    //All info is ready, time to create struc, open device and add to list
    pp_dev = calloc(sizeof(struct portpilot_dev), 1);

    if (!pp_dev) {
        fprintf(stderr, "Failed to allocate memory for PortPilot device\n");
        return 0;
    }

    pp_dev->max_packet_size = max_packet_size;
    pp_dev->input_endpoint = input_endpoint;

    retval = libusb_open(device, &(pp_dev->handle));

    if (retval) {
        fprintf(stderr, "Failed to open device: %s\n",
                libusb_error_name(retval));
        free(pp_dev);
        return 0;
    }

    if (libusb_kernel_driver_active(pp_dev->handle, intf_num) == 1) {
        retval = libusb_detach_kernel_driver(pp_dev->handle, intf_num);

        if (retval) {
            fprintf(stderr, "Failed to detach kernel driver: %s\n",
                    libusb_error_name(retval));
            libusb_close(pp_dev->handle);
            free(pp_dev);
            return 0;
        }
    }

    retval = libusb_claim_interface(pp_dev->handle, intf_num);

    if (retval) {
        fprintf(stderr, "Failed to claim interface: %s\n",
                libusb_error_name(retval));
        libusb_close(pp_dev->handle);
        free(pp_dev);
        return 0;
    }

    //Read and store serial number
    //TODO: Do this earlier when we add support for listening to single device
    libusb_get_device_descriptor(device, &desc);
    retval = libusb_get_string_descriptor_ascii(pp_dev->handle,
            desc.iSerialNumber, pp_dev->serial_number, MAX_USB_STR_LEN);

    if (retval < 0) {
        fprintf(stderr, "Failed to get serial number: %s\n",
                libusb_error_name(retval));
        libusb_close(pp_dev->handle);
        free(pp_dev);
        return 0;
    }

    //Will be a list insert
    pp_ctx->dev = pp_dev;

    //Ready to start reading
    fprintf(stdout, "Ready to start reading on device %s\n",
            pp_dev->serial_number);
    
    return 0;
}

static int portpilot_event_added(libusb_device *device,
        struct portpilot_ctx *pp_ctx)
{

    uint8_t conf_desc_idx, input_endpoint, intf_num;
    uint16_t max_packet_size;
    int32_t retval = 0, intf_desc_idx;
    struct libusb_device_descriptor desc = {0};
    struct libusb_config_descriptor *conf_desc = NULL;
    const struct libusb_interface_descriptor *intf_desc;

    if (pp_ctx->dev)
        return 0;

    libusb_get_device_descriptor(device, &desc);

    //Without a serial number, we can't identify device (should not happen for
    //portpilot)
    if (!desc.iSerialNumber) {
        fprintf(stderr, "Device serial number missing\n");
        return 0;
    }

    libusb_get_active_config_descriptor(device, &conf_desc);

    if (!conf_desc) {
        fprintf(stderr, "Failed to get config descriptor\n");
        return 0;
    }

    retval = portpilot_get_hid_idx(conf_desc, &conf_desc_idx, &intf_desc_idx);

    if (!retval) {
        fprintf(stderr, "Failed to get index for HID device\n");
        libusb_free_config_descriptor(conf_desc);
        return 0;
    }

    intf_desc = &(conf_desc->interface[conf_desc_idx].
            altsetting[intf_desc_idx]);
    intf_num = intf_desc->bInterfaceNumber;

    retval = portpilot_get_input_info(intf_desc, &input_endpoint,
            &max_packet_size);

    if (!retval) {
        fprintf(stderr, "Failed to get input endpoint info\n");
        libusb_free_config_descriptor(conf_desc);
        return 0;
    }

    libusb_free_config_descriptor(conf_desc);
    
    return portpilot_create_dev(device, pp_ctx, max_packet_size, input_endpoint,
            intf_num);
}

//Generic device callback
int portpilot_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data)
{
    struct portpilot_ctx *pp_ctx = user_data;

    //Decide how to handle this later
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
        return portpilot_event_left(device, pp_ctx);
    else
        return portpilot_event_added(device, pp_ctx);
}

static uint8_t portpilot_configure(struct portpilot_ctx *ppc)
{
    const struct libusb_pollfd **libusb_fds;
    const struct libusb_pollfd *libusb_fd;
    int32_t i = 0;

    ppc->event_loop = backend_event_loop_create();
    
    if (!ppc->event_loop) {
        fprintf(stderr, "Failed to allocate event loop\n");
        return RETVAL_FAILURE;
    }

    libusb_fds = libusb_get_pollfds(NULL);

    if (!libusb_fds) {
        fprintf(stderr, "Failed to get libusb fds\n");
        return RETVAL_FAILURE;
    }

    //TODO: Add callback
    ppc->libusb_handle = backend_create_epoll_handle(ppc, 0,
                portpilot_event_cb, 1);   

    if (!ppc->libusb_handle) {
        fprintf(stderr, "Failed to create libusb handle\n");
        return RETVAL_FAILURE;
    }

    libusb_fd = libusb_fds[i];

    while (libusb_fd) {
        backend_event_loop_update(ppc->event_loop,
                                  libusb_fd->events,
                                  EPOLL_CTL_ADD,
                                  libusb_fd->fd,
                                  ppc->libusb_handle);
        libusb_fd = libusb_fds[++i];
    }

    free(libusb_fds);

    libusb_set_pollfd_notifiers(NULL,
                                portpilot_libusb_fd_add,
                                portpilot_libusb_fd_remove,
                                ppc);

    libusb_hotplug_register_callback(NULL,
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                     LIBUSB_HOTPLUG_ENUMERATE,
                                     PORTPILOT_VID,
                                     PORTPILOT_PID,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     portpilot_cb,
                                     ppc, NULL);

    libusb_lock_events(NULL);

    return RETVAL_SUCCESS;
}

int main(int argc, char *argv[])
{
    struct timeval tv;
    uint64_t cur_time;
    struct portpilot_ctx *ppc;
    int retval;

    ppc = calloc(sizeof(struct portpilot_ctx), 1);

    if (!ppc) {
        fprintf(stderr, "Failed to allocate memory for context\n");
        exit(EXIT_FAILURE);
    }

    //Global libusb initsialisation
    retval = libusb_init(NULL);

    if (retval) {
        fprintf(stderr, "libusb failed with error %s\n",
                libusb_error_name(retval));
        exit(EXIT_FAILURE);
    }

    if (!portpilot_configure(ppc)) {
        fprintf(stderr, "Failed to configure struct\n");
        exit(EXIT_FAILURE);
    }

    gettimeofday(&tv, NULL);
    cur_time = (tv.tv_sec * 1e3) + (tv.tv_usec / 1e3);

    //TODO: Fix this later to proerly consider libusb timeouts
    if (!backend_event_loop_add_timeout(ppc->event_loop, cur_time + 1000,
                portpilot_itr_cb, ppc, 1000)) {
        fprintf(stderr, "Failed to add libusb timeout timer\n");
        exit(EXIT_FAILURE);
    }

    backend_event_loop_run(ppc->event_loop);

    //Graceful exit
    libusb_exit(NULL);

    exit(EXIT_FAILURE);
}
