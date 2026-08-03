// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

// temp storage
#define DEVICE_NAME "cache_flush"
#define BUFFER_SIZE 256

static char kernel_buffer[BUFFER_SIZE] __aligned(64); // Use 64 bytes as the cache-block size for Banana Pi K1
static size_t data_size;

static inline void do_cbo_flush(void *addr) {
	asm volatile(
			"cbo.flush 0(%0)"
			:
			: "r"(addr)
			: "memory"
		    );
}

// ---- open() ----
static int cache_open(struct inode *inode, struct file *file) {
	printk(KERN_INFO "cache_flush: device opened\n");
	return 0;
}

// ---- close() ----
static int cache_release(struct inode *inode, struct file *file) {
	printk(KERN_INFO "cache_flush: device closed\n");
	return 0;
}

// ---- write() ----
static ssize_t cache_write(struct file *file, const char __user *user_buffer, size_t count, loff_t *offset) {
	if (count > BUFFER_SIZE)
		count = BUFFER_SIZE;

	if (copy_from_user(kernel_buffer, user_buffer, count))
		return -EFAULT;
	data_size = count;

	// start to invalidate the cache content at kernel_buffer address
	printk(KERN_INFO "cache_flush: recieved %zu bytes\n", count);

	printk(KERN_INFO "cache_flush: ececuting cbo.flush on %px\n");

	do_cbo_flush(kernel_buffer);

	printk(KERN_INFO "cache_flush: cbo.flush completed\n");
	return count;
}

// ---- read() ----
static ssize_t cache_read(struct file *file, char __user *user_buffer, size_t count, loff_t *offset) {
	return simple_read_from_buffer(user_buffer, count, offset, kernel_buffer, data_size);
}

// ---- file operation ----
static const struct file_operations cache_fops = {
	.owner = THIS_MODULE,
	.open = cache_open,
	.release = cache_release,
	.read = cache_read,
	.write = cache_write,
};

// ---- misc device ----
static struct miscdevice cache_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &cache_fops,
	.mode = 06666,
};

// ---- module init ----
static int __init cache_init(void) {
	printk(KERN_INFO "cache_flush: loading driver\n");

	return misc_register(&cache_device);
}

// ---- module exit ----
static void __exit cache_exit(void) {
	misc_deregister(&cache_device);

	printk(KERN_INFO "cache_flush: driver unloaded\n");
}

module_init(cache_init);
module_exit(cache_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Amir");
MODULE_DESCRIPTION("Mnimal cache flush communication driver"); 
