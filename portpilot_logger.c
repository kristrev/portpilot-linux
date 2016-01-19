#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libusb-1.0/libusb.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <string.h>

#include "portpilot_logger.h"
#include "portpilot_callbacks.h"
#include "backend_event_loop.h"

//In the main file
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
                portpilot_cb_event_cb, 1);   

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
                                portpilot_cb_libusb_fd_add,
                                portpilot_cb_libusb_fd_remove,
                                ppc);

    libusb_hotplug_register_callback(NULL,
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                     LIBUSB_HOTPLUG_ENUMERATE,
                                     PORTPILOT_VID,
                                     PORTPILOT_PID,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     portpilot_cb_libusb_cb,
                                     ppc, NULL);

    libusb_lock_events(NULL);

    return RETVAL_SUCCESS;
}

void portpilot_logger_start_itr_cb(struct portpilot_ctx *pp_ctx)
{
    if (!pp_ctx->num_itr_req)
        pp_ctx->event_loop->itr_cb = portpilot_cb_itr_cb;

    ++pp_ctx->num_itr_req;
}

void portpilot_logger_stop_itr_cb(struct portpilot_ctx *pp_ctx)
{
    --pp_ctx->num_itr_req;

    if (!pp_ctx->num_itr_req)
        pp_ctx->event_loop->itr_cb = NULL;
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
                portpilot_cb_itr_cb, ppc, 1000)) {
        fprintf(stderr, "Failed to add libusb timeout timer\n");
        exit(EXIT_FAILURE);
    }

    //ppc->event_loop->itr_cb = portpilot_itr_cb;
    backend_event_loop_run(ppc->event_loop);

    //Graceful exit
    libusb_exit(NULL);

    exit(EXIT_FAILURE);
}
