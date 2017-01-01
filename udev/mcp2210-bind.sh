#!/bin/sh

# A simplified script to rebind a MCP2210 device to the correct driver via udev
# Usage: mcp2210-bind.sh KERNEL_NAME(%k) SYSFS(%S)

modprobe mcp2210
echo "$1" > "$2/bus/usb/drivers/usbhid/unbind"
echo "$1" > "$2/bus/usb/drivers/mcp2210/bind"