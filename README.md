# QEMU edu PCI driver

An out-of-tree Linux PCI driver for QEMU's emulated edu device (`1234:11e8`), with a minimal QEMU and BusyBox guest for testing.

The driver maps BAR0 and reads the identification and liveness registers. During probe, it calculates a factorial using an MSI interrupt and a threaded handler, then performs a 64-byte DMA round trip between guest memory and the device's internal buffer. The returned data is checked byte by byte. These are probe-time tests; there is no user-space device interface yet.

## Requirements

- An x86-64 Linux host with QEMU (`qemu-system-x86_64`), KVM access, and standard kernel build tools.
- A configured and built x86-64 Linux kernel in `build/`, including PCI, MSI, modules, initramfs and serial console support. The lab was tested with Linux 7.4-rc1.
- A statically linked x86-64 BusyBox binary at `rootfs/bin/busybox`, with the applets used by `rootfs/init` enabled.
- `cpio` and `gzip` to create the initramfs.

`linux/`, `build/`, the BusyBox binary, and the generated initramfs are not tracked. The module must be built against the same configured kernel that the guest boots.

## Kernel and guest setup

From the repository root, using an existing Linux source checkout as an example:

```sh
git -C ~/src/linux worktree add --detach "$PWD/linux" v7.4-rc1
make -C linux O="$PWD/build" x86_64_defconfig
make -C linux O="$PWD/build" -j"$(nproc)"
mkdir -p rootfs/bin
```

Place a statically linked BusyBox binary at `rootfs/bin/busybox` and make it executable. Check it with `file rootfs/bin/busybox`. The guest's `init` installs its applet links, mounts `proc`, `sysfs`, and `devtmpfs`, and starts a shell on the serial console.

## Build and run

From the repository root:

```sh
make -C driver
cp driver/edu.ko rootfs/root/
./mkinitramfs.sh
./run.sh
```

Rebuild the initramfs after copying an updated module. `run.sh` boots `build/arch/x86/boot/bzImage` with `-device edu`, KVM, and a serial console. Exit QEMU with `Ctrl-A`, then `X`.

## Test in the guest

```sh
insmod /root/edu.ko
dmesg | tail
rmmod edu
```

The kernel log should include the device identification, an inverted liveness value, `factorial: 5! = 120 (MSI)`, and `DMA round trip: 64 bytes verified`. The out-of-tree module taint message is expected. The driver releases its resources when unloaded.

The DMA test polls for completion with a timeout. If it reports that DMA timed out, reboot the guest before retrying; the driver retains the buffer because the device may still be using it.

## References

- [QEMU edu device specification](https://www.qemu.org/docs/master/specs/edu.html)
- [Linux PCI driver documentation](https://docs.kernel.org/PCI/pci.html)
- [Linux DMA API guide](https://docs.kernel.org/core-api/dma-api-howto.html)
