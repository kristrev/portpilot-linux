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

#ifndef PORTPILOT_HELPERS_H
#define PORTPILOT_HELPERS_H

#include <stdint.h>

struct libusb_config_descriptor;
struct libusb_interface_descriptor;
struct portpilot_ctx;
struct portpilot_dev;
struct portpilot_data;

//Get index of HID device we will communicate with
uint8_t portpilot_helpers_get_hid_idx(const struct libusb_config_descriptor *conf_desc,
        uint8_t *conf_desc_idx, int32_t *intf_desc_idx);

//Get endpoint that we will communicate with, in addition to packets
uint8_t portpilot_helpers_get_input_info(const struct libusb_interface_descriptor *intf_desc,
        uint8_t *input_endpoint, uint16_t *max_packet_size);

//Allocate memory for the portpilot_dev pointer, open USB device, etc.
uint8_t portpilot_helpers_create_dev(libusb_device *device,
        struct portpilot_ctx *pp_ctx, uint16_t max_packet_size,
        uint8_t input_endpoint, uint8_t intf_num, uint8_t *dev_path,
        uint8_t dev_path_len);

//Prepare and submit the first transfer to device
void portpilot_helpers_start_reading_data(struct portpilot_dev *pp_dev);

//Free memory allocate to one device
void portpilot_helpers_free_dev(struct portpilot_dev *pp_dev);

//Extract serial number from usb device (if present) and compare with desired
//serial. Return SUCCESS/FAILURE
uint8_t portpilot_helpers_cmp_serial(const char *desired_serial,
        libusb_device *device);

//Check if a device with the matching pat/path_len exists in device list,
//returns or NULL
struct portpilot_dev* portpilot_helpers_find_dev(
        const struct portpilot_ctx *pp_ctx, const uint8_t *dev_path,
        uint8_t dev_path_len);

//Free all memory occupied by one context (including devie list)
uint8_t portpilot_helpers_free_ctx(struct portpilot_ctx *pp_ctx, uint8_t force);

//Check if all devices are done with receiving the required number of packets
//and stop loop if so
void portpilot_helpers_stop_loop(struct portpilot_ctx *pp_ctx);

//output the data store in pp_data, according to rules specified in the context
//that pp_dev belongs to
void portpilot_helpers_output_data(struct portpilot_dev *pp_dev,
        struct portpilot_data *pp_data);

//increase number of packets received counter and potentially stop event loop
uint8_t portpilot_helpers_inc_num_pkts(struct portpilot_dev *pp_dev);
#endif
