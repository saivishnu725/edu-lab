#!/bin/sh

cd "$(dirname "$0")"

exec qemu-system-x86_64 \
    -enable-kvm -cpu host -m 512M -smp 2 \
    -kernel build/arch/x86/boot/bzImage \
    -initrd initramfs.cpio.gz \
    -append "console=ttyS0 nokaslr" \
    -device edu \
    -nographic

# -enable-kvm   :   near native speed virtualisation
# -smp 2        :   gives guest 2 CPUs (useful later)
# -kernel / -initrd :   boot the kernel and rootfs directly; with no disk image and no bootloader
# console=ttyS0 / -nographic    :   route the guest console into the terminal
# -device edu   :   the edu card
