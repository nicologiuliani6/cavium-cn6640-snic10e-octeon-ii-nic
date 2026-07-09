#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
static int __init hello_init(void){ printk(KERN_INFO "CARDMOD hello: loaded\n"); return 0; }
static void __exit hello_exit(void){ printk(KERN_INFO "CARDMOD hello: unloaded\n"); }
module_init(hello_init); module_exit(hello_exit);
MODULE_LICENSE("GPL");
