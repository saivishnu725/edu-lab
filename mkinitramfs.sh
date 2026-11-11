#!/bin/sh

cd "$(dirname "$0")/rootfs"
find . -print0 | cpio --null -o -H newc --owner root:root | gzip -9 > ../initramfs.cpio.gz

# -H newc is the only cpio format the kernel accepts. WHY?
# --owner root:root makes the files root owned inside the guest, without needing sudo
