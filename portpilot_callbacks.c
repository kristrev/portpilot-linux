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
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#include "portpilot_callbacks.h"
#include "portpilot_logger.h"
#include "backend_event_loop.h"
#include "portpilot_helpers.h"
#include "backend_event_loop.h"

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
    struct portpilot_dev *ppd_itr = pp_ctx->dev_head.lh_first;

    //Run libusb timers
    libusb_unlock_events(NULL);
    libusb_handle_events_timeout_completed(NULL, &tv, NULL);
    libusb_lock_events(NULL);

    //Check if we should stop loop, in case more than one device was connected
    //and one is disconnected after all the others have finished receiving
    //packets
    portpilot_helpers_stop_loop(pp_ctx);

    if (!pp_ctx->num_itr_req)
        return;

    //Restart reading on devices that for some reason have failed
    while (ppd_itr != NULL) {
        if (ppd_itr->read_state == READ_STATE_FAILED_START)
            portpilot_helpers_start_reading_data(ppd_itr);

        ppd_itr = ppd_itr->next_dev.le_next;
    }
}

void portpilot_cb_output_cb(void *ptr)
{
    struct portpilot_ctx *pp_ctx = ptr;
    struct portpilot_dev *ppd_itr = pp_ctx->dev_head.lh_first;

    while (ppd_itr != NULL) {
        if (ppd_itr->agg_data->num_readings) {
            portpilot_helpers_output_data(ppd_itr, ppd_itr->agg_data);
            memset(ppd_itr->agg_data, 0, sizeof(struct portpilot_data));
            portpilot_helpers_inc_num_pkts(ppd_itr);
        }

        ppd_itr = ppd_itr->next_dev.le_next;
    }
}

void portpilot_cb_cancel_cb(void *ptr)
{
    struct portpilot_ctx *pp_ctx = ptr;
    backend_event_loop_stop(pp_ctx->event_loop);
}

void portpilot_cb_event_cb(void *ptr, int32_t fd, uint32_t events)
{
    struct timeval tv = {0 ,0};

    libusb_unlock_events(NULL);
    libusb_handle_events_timeout_completed(NULL, &tv, NULL);
    libusb_lock_events(NULL);
}

static void portpilot_cb_handle_event_left(struct portpilot_ctx *pp_ctx,
        struct portpilot_dev *pp_dev)
{
    if (pp_dev->serial_number)
        fprintf(stderr, "Will remove device with serial number %s\n",
                pp_dev->serial_number);
    else
        fprintf(stderr, "Will remove device\n");

    portpilot_helpers_free_dev(pp_dev);
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

    //If no desired serial number is provided, user has indicated he or she is
    //interested in all portpilots connected. So no need to check for serial
    //etc.
    if (pp_ctx->desired_serial &&
            !portpilot_helpers_cmp_serial(pp_ctx->desired_serial, device)) {
        fprintf(stderr, "Serial number mismatch\n");
        return;
    }

    libusb_get_device_descriptor(device, &desc);
    libusb_get_active_config_descriptor(device, &conf_desc);

    if (!conf_desc) {
        fprintf(stderr, "Failed to get config descriptor\n");
        return;
    }

    //Get the index of the HID interface, it is this interface we read from
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

    //Get the endpoint for the interface we will communicate with
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
    struct portpilot_dev *pp_dev = NULL;
    int32_t retval;
    uint8_t dev_path[USB_MAX_PATH];
    uint8_t dev_path_len;

    //Path is used both on add and remove, so read it already here
    dev_path[0] = libusb_get_port_number(device);
    retval = libusb_get_port_numbers(device, dev_path + 1, 7);
    dev_path_len = retval + 1;

    pp_dev = portpilot_helpers_find_dev(pp_ctx, dev_path, dev_path_len);

    if ((event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT && !pp_dev) ||
        (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED && pp_dev)) {
        fprintf(stderr, "Incorrect state\n");
        return 0;
    }

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
        portpilot_cb_handle_event_left(pp_ctx, pp_dev);
    else
        portpilot_cb_handle_event_added(device, pp_ctx, dev_path, dev_path_len);

    return 0;
}

//USB callback
void portpilot_cb_read_cb(struct libusb_transfer *transfer)
{
    struct portpilot_dev *pp_dev = transfer->user_data;
    struct portpilot_ctx *pp_ctx = pp_dev->pp_ctx;
    struct portpilot_pkt *pp_pkt = (struct portpilot_pkt*) transfer->buffer;
    struct portpilot_data pp_data = {0};
    struct portpilot_data *data_ptr = pp_dev->agg_data ?
        pp_dev->agg_data : &pp_data;
    uint8_t i;

    switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        break;
    case LIBUSB_TRANSFER_ERROR:
    case LIBUSB_TRANSFER_TIMED_OUT:
        fprintf(stderr, "Previous transfer failed/timed out, retransmit\n");
        libusb_submit_transfer(transfer);
        return;
    case LIBUSB_TRANSFER_CANCELLED:
        //We only get here if we have cancelled device, thus, we don't need any
        //additional guards
        if (++pp_ctx->num_cancelled == pp_ctx->num_cancel)
            backend_event_loop_stop(pp_ctx->event_loop);
        return;
    default:
        //So far I have only seen this on disconnect, fail silently and then we
        //clean up later
        return;
    }

    //We sometimes see replies that are 0 length, ignore those and just
    //re-submit transfer
    if (!transfer->actual_length) {
        libusb_submit_transfer(transfer);
        return;
    }

    if (pp_ctx->verbose) {
        for (i = 0; i < transfer->actual_length - 1; i++)
            fprintf(stdout, "%x:", transfer->buffer[i]);
        fprintf(stdout, "%x\n", transfer->buffer[i]);
    }
   
    //We ignore the direction of the V, A and use absolute values to get a
    //correct sum
    data_ptr->tstamp = pp_pkt->tstamp;
    data_ptr->v_in += pp_pkt->v_in >= 0 ? pp_pkt->v_in :
        (pp_pkt->v_in * -1);
    data_ptr->v_out += pp_pkt->v_out >= 0 ? pp_pkt->v_out :
        (pp_pkt->v_out * -1);
    data_ptr->current += pp_pkt->current >= 0 ? pp_pkt->current :
        (pp_pkt->current * -1);
    data_ptr->max_current = pp_pkt->max_current >= 0 ? pp_pkt->max_current :
        (pp_pkt->max_current * -1);
    data_ptr->energy += pp_pkt->energy >= 0 ? pp_pkt->energy :
        (pp_pkt->energy * -1);
    data_ptr->total_energy = pp_pkt->total_energy >= 0 ?
        pp_pkt->total_energy / 3600 :
        (pp_pkt->total_energy * -1) / 3600;
    data_ptr->num_readings++;

    //If we output aggregated data, then the timeout callback is responsible for
    //the output, stopping the loop etc.
    if (pp_dev->agg_data) {
        libusb_submit_transfer(transfer);
        return;
    }

    portpilot_helpers_output_data(pp_dev, data_ptr);

    //Only submit transfer if we have not exceeded packet limit
    if (!portpilot_helpers_inc_num_pkts(pp_dev)) {
        libusb_submit_transfer(transfer);
    }
}
