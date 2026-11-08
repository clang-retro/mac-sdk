#!/bin/sh
# Boot System 7.6.1 or Mac OS 8.1 on QEMU's q800 (Quadra 800) machine,
# straight from a copy of the install CD. Put the app on the copy first:
#
#   cp "Mac OS 7.6.1.iso" cd.img
#   hmount cd.img 1 && hcopy -m Hello68.bin :Hello68 && humount
#   Q800_ROM=q800.rom OS9_DIR=. sh boot-q800.sh cd.img
#
# Needs a Quadra 800 ROM image (checksum F1ACAD13 in its first 4 bytes),
# which the SDK cannot supply. Screen on VNC :2; monitor on a Unix socket
# that mon.py drives (OS9_DIR picks its directory). In the Finder, type
# the app's name to select it and press Command-O (mon.py: key meta_l-o).
cd_img=${1:?usage: boot-q800.sh CD.img}
dir=${OS9_DIR:-.}
exec qemu-system-m68k -M q800 -m 128 -bios "${Q800_ROM:?set Q800_ROM}" \
  -g 800x600x8 \
  -drive file="$cd_img",format=raw,media=cdrom,if=none,id=cd0 \
  -device scsi-cd,scsi-id=3,drive=cd0 \
  -audiodev none,id=snd -machine audiodev=snd \
  -display none -vnc :2 -monitor unix:"$dir/mon.sock",server,nowait
