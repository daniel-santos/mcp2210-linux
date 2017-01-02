#!/bin/sh

if [ "$#" != 3 ] ; then
	echo "Usage: $0 sysfs_path kernel_name new_driver_path" >&2
	echo "" >&2
	echo "To be used in udev rules:" >&2
	echo "   RUN+=\"$0 %k %S%p %S/bus/usb/drivers/mcp2210\"" >&2
	echo "which rebinds a particular device to a new driver." >&2
	exit 1
fi

set -e
logger -t "$0" "Rebind device $1($2) to driver $3."

echo "$2" >> "$1/driver/unbind"
sleep 1
echo "$2" >> "$3/bind"
