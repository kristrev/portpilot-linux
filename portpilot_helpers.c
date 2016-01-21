/*
 * Copyright 2016 Kristian Evensen <kristian.evensen@gmail.com>
 *
 * This file is part of Portpilot Logger. Portpilot Logger is free software: you
 * can redistribute it and/or modify it under the terms of the Lesser GNU
 * General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Portpilot Logger is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Portpilot Logger. If not, see http://www.gnu.org/licenses/.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#include "portpilot_helpers.h"
#include "portpilot_logger.h"
#include "portpilot_callbacks.h"
#include "backend_event_loop.h"

static uint8_t portpilot_helpers_get_serial_num(libusb_device *device,
        unsigned char *serial_buf, uint8_t serial_buf_len)
{
    struct libusb_device_descriptor desc = {0};
    struct libusb_device_handle *handle;
    int32_t retval;

    libusb_get_device_descriptor(device, &desc);

    //No device number to extract
    if (!desc.iSerialNumber) {
        fprintf(stderr, "Device serial number missing\n");
        return RETVAL_FAILURE;
    }

    retval = libusb_open(device, &handle);

    if (retval) {
        fprintf(stderr, "Failed to open device: %s\n",
                libusb_error_name(retval));
        return RETVAL_FAILURE;
    }

    retval = libusb_get_string_descriptor_ascii(handle,
            desc.iSerialNumber, serial_buf, serial_buf_len);

    if (retval < 0) {
        fprintf(stderr, "Failed to get serial number: %s\n",
                libusb_error_name(retval));
        libusb_close(handle);
        return RETVAL_FAILURE;
    }

    libusb_close(handle);
    return RETVAL_SUCCESS;
}

//Get the indexes of the first HID interface. It is this interface we use to
//communicate with the Portpilot
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

//Get the info on the input endpoint (the ones we read from)
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

void portpilot_helpers_free_dev(struct portpilot_dev *pp_dev)
{
    libusb_release_interface(pp_dev->handle, pp_dev->intf_num);
    libusb_close(pp_dev->handle);

    //It seems that if a device is disconnected, transfer fails before device is
    //removed, so we clean up memory correctly. In the context case, we make
    //sure that transfer is not active before calling free_dev
    if (pp_dev->transfer)
        libusb_free_transfer(pp_dev->transfer);

    if (pp_dev->agg_data)
        free(pp_dev->agg_data);

    --pp_dev->pp_ctx->dev_list_len;
    LIST_REMOVE(pp_dev, next_dev);
    free(pp_dev);
}

uint8_t portpilot_helpers_create_dev(libusb_device *device,
        struct portpilot_ctx *pp_ctx, uint16_t max_packet_size,
        uint8_t input_endpoint, uint8_t intf_num, uint8_t *dev_path,
        uint8_t dev_path_len)
{
    int32_t retval;
    struct portpilot_dev *pp_dev = NULL;

    //TODO: Split into new function?
    //All info is ready, time to create struc, open device and add to list
    pp_dev = calloc(sizeof(struct portpilot_dev), 1);

    if (!pp_dev) {
        fprintf(stderr, "Failed to allocate memory for PortPilot device\n");
        return RETVAL_FAILURE;
    }

    if (pp_ctx->output_interval) {
        pp_dev->agg_data = calloc(sizeof(struct portpilot_data), 1);

        if (!pp_dev->agg_data) {
            fprintf(stderr, "Failed to allocate memory for agg. data\n");
            return RETVAL_FAILURE;
        }
    }

    pp_dev->max_packet_size = max_packet_size;
    pp_dev->input_endpoint = input_endpoint;
    pp_dev->intf_num = intf_num;

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

    //Try to read serial number from device. It is not critical if it is not
    //present, the check for matching a desired serial has been done by the time
    //we get here
    portpilot_helpers_get_serial_num(device, pp_dev->serial_number,
            MAX_USB_STR_LEN);

    memcpy(pp_dev->path, dev_path, dev_path_len);
    pp_dev->path_len = dev_path_len;

    pp_dev->pp_ctx = pp_ctx;
    LIST_INSERT_HEAD(&(pp_dev->pp_ctx->dev_head), pp_dev, next_dev);
    ++pp_ctx->dev_list_len;

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

uint8_t portpilot_helpers_cmp_serial(const char *desired_serial,
        libusb_device *device)
{
    size_t desired_serial_len = strlen(desired_serial);
    uint8_t dev_serial_number[MAX_USB_STR_LEN + 1] = {0};

    if (!portpilot_helpers_get_serial_num(device, dev_serial_number,
                MAX_USB_STR_LEN))
        return RETVAL_FAILURE;

    if (strlen((const char*) dev_serial_number) == desired_serial_len &&
        !strncmp((const char*) dev_serial_number, desired_serial,
            desired_serial_len))
        return RETVAL_SUCCESS;
    else
        return RETVAL_FAILURE;
}

struct portpilot_dev* portpilot_helpers_find_dev(
        const struct portpilot_ctx *pp_ctx, const uint8_t *dev_path,
        uint8_t dev_path_len)
{
    struct portpilot_dev *ppd_itr = pp_ctx->dev_head.lh_first;

    while (ppd_itr != NULL) {
        if (ppd_itr->path_len == dev_path_len &&
            !memcmp(ppd_itr->path, dev_path, dev_path_len))
            return ppd_itr;

        ppd_itr = ppd_itr->next_dev.le_next;
    }

    return NULL;
}

uint8_t portpilot_helpers_free_ctx(struct portpilot_ctx *pp_ctx, uint8_t force)
{
    struct portpilot_dev *ppd_itr = pp_ctx->dev_head.lh_first, *ppd_tmp;
    uint8_t failed_cancels = 0;

    while (ppd_itr != NULL) {
        ppd_tmp = ppd_itr;
        ppd_itr = ppd_itr->next_dev.le_next;

        //We are only allowed to free memory if transfer is cancelled, so check
        //for this and indicate to loop if we need to wait for cancelled
        //transfers
        if (force || (ppd_tmp->transfer &&
                libusb_cancel_transfer(ppd_tmp->transfer) ==
                LIBUSB_ERROR_NOT_FOUND)) {
            portpilot_helpers_free_dev(ppd_tmp);
        } else {
            ++pp_ctx->num_cancel;
            ++failed_cancels;
        }
    }

    if (failed_cancels)
        return failed_cancels;

    if (pp_ctx->output_timeout_handle)
        free(pp_ctx->output_timeout_handle);

    free(pp_ctx->itr_timeout_handle);
    free(pp_ctx->libusb_handle);
    free(pp_ctx->event_loop);
    free(pp_ctx);

    return failed_cancels;
}

void portpilot_helpers_stop_loop(struct portpilot_ctx *pp_ctx)
{
    //TODO: Consider how to handle removal of devices when we are going to read
    //a specified number of packets
    if (pp_ctx->dev_list_len && pp_ctx->num_done_read == pp_ctx->dev_list_len)
        backend_event_loop_stop(pp_ctx->event_loop);
}

void portpilot_helpers_output_data(struct portpilot_dev *pp_dev,
        struct portpilot_data *pp_data)
{
    struct portpilot_ctx *pp_ctx = pp_dev->pp_ctx;
    const uint8_t *serial_number = pp_dev->serial_number ?
        pp_dev->serial_number : (const uint8_t *) "";

    if (pp_ctx->csv_output) 
        fprintf(stdout, "%s,%u,%u,%u,%u,%u,%u,%u\n",
            serial_number,
            pp_data->tstamp,
            pp_data->v_in/pp_data->num_readings,
            pp_data->v_out/pp_data->num_readings,
            pp_data->current/pp_data->num_readings,
            pp_data->max_current,
            pp_data->energy/pp_data->num_readings,
            pp_data->total_energy);
    else
        fprintf(stdout, "Serial %s, tstamp %usec, v_in %umV, v_out %u mV"
            ", current %umA, max. current %umA, energy %umW"
            ", total energy %umWh\n",
            serial_number,
            pp_data->tstamp,
            pp_data->v_in/pp_data->num_readings,
            pp_data->v_out/pp_data->num_readings,
            pp_data->current/pp_data->num_readings,
            pp_data->max_current,
            pp_data->energy/pp_data->num_readings,
            pp_data->total_energy);

    //TODO: Consider how to handle errors when writing to file
    if (pp_ctx->output_file)
        fprintf(stdout, "%s,%u,%u,%u,%u,%u,%u,%u\n",
            serial_number,
            pp_data->tstamp,
            pp_data->v_in/pp_data->num_readings,
            pp_data->v_out/pp_data->num_readings,
            pp_data->current/pp_data->num_readings,
            pp_data->max_current,
            pp_data->energy/pp_data->num_readings,
            pp_data->total_energy);
}

uint8_t portpilot_helpers_inc_num_pkts(struct portpilot_dev *pp_dev)
{
    ++pp_dev->num_pkts;

    if (pp_dev->pp_ctx->pkts_to_read &&
            pp_dev->num_pkts == pp_dev->pp_ctx->pkts_to_read) {
        pp_dev->pp_ctx->num_done_read++;
        portpilot_helpers_stop_loop(pp_dev->pp_ctx);
        return RETVAL_SUCCESS;
    }

    return RETVAL_FAILURE;
}
