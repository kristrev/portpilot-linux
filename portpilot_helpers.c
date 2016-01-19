#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#include "portpilot_helpers.h"
#include "portpilot_logger.h"
#include "portpilot_callbacks.h"

//Get the indexes of the HID interface
uint8_t portpilot_helpers_get_hid_idx(const struct libusb_config_descriptor *conf_desc,
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

//Get the info on the input endpoint (the one we read from)
uint8_t portpilot_helpers_get_input_info(const struct libusb_interface_descriptor *intf_desc,
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

uint8_t portpilot_helpers_create_dev(libusb_device *device,
        struct portpilot_ctx *pp_ctx, uint16_t max_packet_size,
        uint8_t input_endpoint, uint8_t intf_num, uint8_t *dev_path,
        uint8_t dev_path_len)
{
    int32_t retval;
    struct portpilot_dev *pp_dev = NULL;
    struct libusb_device_descriptor desc = {0};

    //TODO: Split into new function?
    //All info is ready, time to create struc, open device and add to list
    pp_dev = calloc(sizeof(struct portpilot_dev), 1);

    if (!pp_dev) {
        fprintf(stderr, "Failed to allocate memory for PortPilot device\n");
        return RETVAL_FAILURE;
    }

    pp_dev->max_packet_size = max_packet_size;
    pp_dev->input_endpoint = input_endpoint;

    retval = libusb_open(device, &(pp_dev->handle));

    if (retval) {
        fprintf(stderr, "Failed to open device: %s\n",
                libusb_error_name(retval));
        free(pp_dev);
        return RETVAL_FAILURE;
    }

    if (libusb_kernel_driver_active(pp_dev->handle, intf_num) == 1) {
        retval = libusb_detach_kernel_driver(pp_dev->handle, intf_num);

        if (retval) {
            fprintf(stderr, "Failed to detach kernel driver: %s\n",
                    libusb_error_name(retval));
            libusb_close(pp_dev->handle);
            free(pp_dev);
            return RETVAL_FAILURE;
        }
    }

    retval = libusb_claim_interface(pp_dev->handle, intf_num);

    if (retval) {
        fprintf(stderr, "Failed to claim interface: %s\n",
                libusb_error_name(retval));
        libusb_close(pp_dev->handle);
        free(pp_dev);
        return RETVAL_FAILURE;
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
        return RETVAL_FAILURE;
    }

    memcpy(pp_dev->path, dev_path, dev_path_len);
    pp_dev->path_len = dev_path_len;

    //Will be a list insert
    pp_ctx->dev = pp_dev;
    pp_dev->pp_ctx = pp_ctx;

    //Ready to start reading
    fprintf(stdout, "Ready to start reading on device %s\n",
            pp_dev->serial_number);

    portpilot_helpers_start_reading_data(pp_dev);

    return RETVAL_SUCCESS;
}

static void portpilot_set_read_start_failed(struct portpilot_dev *pp_dev)
{
    portpilot_logger_start_itr_cb(pp_dev->pp_ctx);
    pp_dev->read_state = READ_STATE_FAILED_START; 
}

void portpilot_helpers_start_reading_data(struct portpilot_dev *pp_dev)
{
    int32_t retval;

    if (!pp_dev->read_buf)
        pp_dev->read_buf = calloc(pp_dev->max_packet_size, 1);

    //Failed to allocate buffer, must indicate to loop
    if (!pp_dev->read_buf) {
        fprintf(stderr, "Failed to allocate read buffer\n");

        if (pp_dev->read_state != READ_STATE_FAILED_START)
            portpilot_set_read_start_failed(pp_dev);

        return;
    }

    if (!pp_dev->transfer) {
        pp_dev->transfer = libusb_alloc_transfer(0);
        pp_dev->transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER;
    }

    if (!pp_dev->transfer) {
        fprintf(stderr, "Failed to allocate libusb transfer\n");

        if (pp_dev->read_state != READ_STATE_FAILED_START)
            portpilot_set_read_start_failed(pp_dev);

        return;
    }

    libusb_fill_interrupt_transfer(pp_dev->transfer, pp_dev->handle,
            pp_dev->input_endpoint, (unsigned char*) pp_dev->read_buf,
            pp_dev->max_packet_size, portpilot_cb_read_cb, pp_dev, 5000);

    retval = libusb_submit_transfer(pp_dev->transfer);

    //Don't consider an already transfered transfer a failure. This should not
    //really happen now, but it could be that we want to do something smart wrt
    //caching and so on later
    if (retval && retval != LIBUSB_ERROR_BUSY) {
        fprintf(stderr, "Failed to submit transfer\n");

        if (pp_dev->read_state != READ_STATE_FAILED_START)
            portpilot_set_read_start_failed(pp_dev);

        return;
    }

    if (pp_dev->read_state == READ_STATE_FAILED_START)
        portpilot_logger_stop_itr_cb(pp_dev->pp_ctx);

    pp_dev->read_state = READ_STATE_RUNNING;
}