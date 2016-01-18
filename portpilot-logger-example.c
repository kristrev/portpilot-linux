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
    printf("tstamp: %us v_in: %umV v_out: %umV current: %umA max. current: %umA total energy: %umWh power: %umW\n",
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

//Generic device callback
int portpilot_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data)
{
    int i, j, retval;
    int32_t interface_number = -1;
    uint8_t is_interrupt = 0, is_input = 0;
    struct portpilot_ctx *ppc = user_data;
    struct libusb_config_descriptor *conf_desc = NULL;
    const struct libusb_interface *intf;
    const struct libusb_interface_descriptor *intf_desc;
    const struct libusb_endpoint_descriptor *ep;

    //Decide how to handle this later
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
        exit(EXIT_FAILURE);

    if (ppc->dev_handle)
        return 0;

    libusb_get_active_config_descriptor(device, &conf_desc); 

    if (!conf_desc) {
        fprintf(stderr, "Failed to get configuration\n");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < conf_desc->bNumInterfaces; i++) {
        intf = &conf_desc->interface[i];

        for (j = 0; j < intf->num_altsetting; j++) {
            intf_desc = &intf->altsetting[j];

            if (intf_desc->bInterfaceClass == LIBUSB_CLASS_HID) {
                interface_number = intf_desc->bInterfaceNumber;
                break;
            }
        }

        if (interface_number >= 0)
            break;
    }
  

    if (interface_number == -1) {
        libusb_free_config_descriptor(conf_desc);
        return 0;
    }

    retval = libusb_open(device, &(ppc->dev_handle));

    if (retval) {
        fprintf(stderr, "Failed to open device %s\n",
                libusb_error_name(retval));
        libusb_free_config_descriptor(conf_desc);
        exit(EXIT_FAILURE);
    }

    if (libusb_kernel_driver_active(ppc->dev_handle, interface_number) == 1) {
        retval = libusb_detach_kernel_driver(ppc->dev_handle, interface_number);

        if (retval) {
            fprintf(stderr, "Failed to detach kernel driver %s\n",
                    libusb_error_name(retval));
            libusb_free_config_descriptor(conf_desc);
            exit(EXIT_FAILURE);
        }
    }

    retval = libusb_claim_interface(ppc->dev_handle, interface_number);

    if (retval) {
        fprintf(stderr, "Failed to claim interface %s\n",
                libusb_error_name(retval));
        libusb_free_config_descriptor(conf_desc);
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < intf_desc->bNumEndpoints; i++) {
        ep = &intf_desc->endpoint[i];

        is_interrupt = (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_INTERRUPT;
        is_input = (ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;

        if (is_interrupt && is_input) {
            ppc->input_endpoint = ep->bEndpointAddress;
            ppc->max_packet_size = ep->wMaxPacketSize;
        }
    }

    libusb_free_config_descriptor(conf_desc);
    portpilot_read_data(ppc);

    return 0;
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
                                     0x16d0,
                                     0x08ac,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     portpilot_cb,
                                     ppc, NULL);

    libusb_lock_events(NULL);

    return RETVAL_SUCCESS;
}

int main(char *argv[], int argc)
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
    libusb_exit(NULL);

    exit(EXIT_FAILURE);
}
