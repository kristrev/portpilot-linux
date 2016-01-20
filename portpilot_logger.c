#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libusb-1.0/libusb.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <string.h>

#include "portpilot_logger.h"
#include "portpilot_callbacks.h"
#include "portpilot_helpers.h"
#include "backend_event_loop.h"

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

static uint8_t portpilot_start(uint32_t num_pkts, const char *serial_number,
        uint8_t verbose, uint8_t csv_output, FILE *output_file)
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

    ppc->pkts_to_read = num_pkts;
    ppc->desired_serial = serial_number;
    ppc->verbose = verbose;
    ppc->csv_output = csv_output;
    ppc->output_file = output_file;

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

    ppc->itr_timeout_handle = backend_event_loop_add_timeout(ppc->event_loop,
            cur_time + 1000, portpilot_cb_itr_cb, ppc, 1000);
        
    if (!ppc->itr_timeout_handle) {
        fprintf(stderr, "Failed to add libusb timeout timer\n");
        exit(EXIT_FAILURE);
    }

    //ppc->event_loop->itr_cb = portpilot_itr_cb;
    backend_event_loop_run(ppc->event_loop);

    if (ppc->event_loop->stop)
        retval = RETVAL_SUCCESS;
    else
        retval = RETVAL_FAILURE;

    if (ppc->dev)
        portpilot_helpers_free_dev(ppc->dev);

    //Graceful exit
    libusb_exit(NULL);
    free(ppc->itr_timeout_handle);
    free(ppc->libusb_handle);
    free(ppc->event_loop);
    free(ppc);

    return (uint8_t) retval;
}

static void usage()
{
    fprintf(stdout, "Supported parameters:\n");
    fprintf(stdout, "\t-r: number of packes to print (default: infinite)\n");
    fprintf(stdout, "\t-d: serial number of device to poll (default: poll "
            "all/first device\n)");
    fprintf(stdout, "\t-v: verbose (print raw USB message)\n");
    fprintf(stdout, "\t-c: print csv to console (no units appended\n");
    fprintf(stdout, "\t-f: write csv to file with specified filename\n");
    fprintf(stdout, "\t-h: this menu\n");
}

int main(int argc, char *argv[])
{
    int32_t opt = 0;
    uint32_t num_pkts = 0;
    const char *serial_number = NULL, *output_filename = NULL;
    uint8_t verbose = 0, csv_output = 0;
    FILE *output_file = NULL;

    while ((opt = getopt(argc, argv, "r:d:f:cvh")) != -1) {
        switch (opt) {
        case 'r':
            num_pkts = (uint32_t) atoi(optarg);
            break;
        case 'd':
            serial_number = optarg;
            break;
        case 'f':
            output_filename = optarg;
            break;
        case 'c':
            csv_output = 1;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
        default:
            usage();
            exit(EXIT_SUCCESS);
        }
    }

    if (output_filename) {
        output_file = fopen(output_filename, "w");

        if (!output_file) {
            fprintf(stderr, "Failed to open desired output file\n");
            exit(EXIT_FAILURE);
        }
    }

    if (output_file && fprintf(output_file, CSV_DESCRIPTION) < 0) {
        fprintf(stderr, "Could not write descriptive row to CSV\n");
        fclose(output_file);
        exit(EXIT_FAILURE);
    }

    opt = portpilot_start(num_pkts, serial_number, verbose, csv_output,
            output_file);

    if (output_file)
        fclose(output_file);

    if (opt)
        exit(EXIT_SUCCESS);
    else
        exit(EXIT_FAILURE);
}
