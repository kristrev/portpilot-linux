Portpilot Logger
================

Portpilot Logger is a tool for reading and outputting data from the [Portpilot
USB Logger](http://portpilot.net/) on Linux. The official software,
unfortunately, only supports Windows and OS X. However, the device was
fortunately very easy to reverse engineer.

The Portpilot exports a HID device and it is this device that data is read from.
Thanks to the fact that the official software supports printing the raw USB
packet, figuring out the data structure was fairly straight forward. A
description is found in `portpilot_logger.h` (`struct portpilot_pkt`).

In default mode, Portpilot Logger will dynamically detect any Portpilot-device
(also those connected while application is running) and start reading data from
them. This makes it possible to easily log the power at multiple places
simultaneously. One use we had for this feature was to check if a USB hub
delievered the promised power.

Requirements
------------

Portpilot Logger depends on libusb and is compiled using cmake. Portpilot Logger
must be run as root in order to work.

Parameters
----------

Portpilot Logger supports the following command line parameters:

* -r X : Stop after X number of packets have been read. Default is infinite.
* -i X : Only output data after X ms. The output is an average of all data that
  has been received in the specified interval.
* -d X : Only read data from interface with serial number X. The default is to
  read from all available devices.
* -v : Verbose mode. Print the raw USB packet.
* -c : Print CSV instead of a more verbose output to console.
* -f X : Write CSV to file X.

The development of Portpilot Logger was funded by the EU-funded research-project
[MONROE](https://www.monroe-project.eu/).
