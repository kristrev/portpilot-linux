#ifndef PORTPILOT_HELPERS_H
#define PORTPILOT_HELPERS_H

#include <stdint.h>

struct libusb_config_descriptor;
struct libusb_interface_descriptor;
struct portpilot_ctx;
struct portpilot_dev;

uint8_t portpilot_helpers_get_hid_idx(const struct libusb_config_descriptor *conf_desc,
        uint8_t *conf_desc_idx, int32_t *intf_desc_idx);
uint8_t portpilot_helpers_get_input_info(const struct libusb_interface_descriptor *intf_desc,
        uint8_t *input_endpoint, uint16_t *max_packet_size);
uint8_t portpilot_helpers_create_dev(libusb_device *device,
        struct portpilot_ctx *pp_ctx, uint16_t max_packet_size,
        uint8_t input_endpoint, uint8_t intf_num, uint8_t *dev_path,
        uint8_t dev_path_len);
void portpilot_helpers_start_reading_data(struct portpilot_dev *pp_dev);

//Free memory allocate to one device
void portpilot_helpers_free_dev(struct portpilot_dev *pp_dev);
#endif
