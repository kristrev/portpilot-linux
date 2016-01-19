#ifndef PORTPILOT_CALLBACKS
#define PORTPILOT_CALLBACKS

void portpilot_cb_libusb_fd_add(int fd, short events, void *data);
void portpilot_cb_libusb_fd_remove(int fd, void *data);
void portpilot_cb_itr_cb(void *ptr);
void portpilot_cb_event_cb(void *ptr, int32_t fd, uint32_t events);

//device event callbac
int portpilot_cb_libusb_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data);

void portpilot_cb_read_cb(struct libusb_transfer *transfer);
#endif
