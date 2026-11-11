// SPDX-LICENSE-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/module.h>

static int __init hello_init(void) {
    pr_info("hello by svnu: module loaded\n");
    return 0;
}

static void __exit hello_exit(void) {
    pr_info("hello by svnu: module unloaded\n");
}

module_init(hello_init); // kernel loads this. 0 means success
module_exit(hello_exit); // when we unload the module

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("my first kernel module for the QEMU edu lab");
