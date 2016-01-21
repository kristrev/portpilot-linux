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

#ifndef PORTPILOT_CALLBACKS
#define PORTPILOT_CALLBACKS

//libusb callbacks for updating event loop
void portpilot_cb_libusb_fd_add(int fd, short events, void *data);
void portpilot_cb_libusb_fd_remove(int fd, void *data);

//our maintenance callback. Called once every second (lazy libusb timeout
//handling) or on every iteration (device has failed to start sending)
void portpilot_cb_itr_cb(void *ptr);

//"default" eventloop callback, called when there is activity on monitored file
//descriptors
void portpilot_cb_event_cb(void *ptr, int32_t fd, uint32_t events);

//libusb hotplug callback (device added/removed)
int portpilot_cb_libusb_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data);

//libusb read callback, i.e., when submitted trasnfer has yielded a result
void portpilot_cb_read_cb(struct libusb_transfer *transfer);

//output callback, called when the interval option is used to output an average
//of the data received since last time
void portpilot_cb_output_cb(void *ptr);

//callback used when cancels are not finished on time. Will just stop event loop
void portpilot_cb_cancel_cb(void *ptr);

#endif
