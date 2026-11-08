#!/bin/sh
# Boot Mac OS 9 in QEMU's mac99 machine with a second, HFS-formatted disk
# for moving files in and out (fill it with hfsutils: hmount xfer.img;
# hcopy -m App.bin :App; humount). Screen on VNC :1; monitor on a Unix
# socket that mon.py drives.
#
#   OS9_DIR=~/os9 sh boot.sh      # needs $OS9_DIR/hd.qcow2 and xfer.img
#
# There is deliberately no usb-tablet: Mac OS 9 has no driver for it, and
# it would take the monitor's mouse events. mon.py selects the USB mouse.
dir=${OS9_DIR:-$HOME/os9}
exec qemu-system-ppc -M mac99,via=pmu -m 512 \
  -drive file="$dir/hd.qcow2",format=qcow2,media=disk \
  -drive file="$dir/xfer.img",format=raw,media=disk \
  -boot c -display none -vnc :1 \
  -monitor unix:"$dir/mon.sock",server,nowait \
  -prom-env 'auto-boot?=true'
