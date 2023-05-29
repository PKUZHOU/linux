#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/cdev.h>

#define DRIVER_NAME "neoprofiler_mmio_driver"
#define DEVICE_NAME "neoprofiler"
#define NEOPROFILER_MMIO_BASE 0x1280000030L
#define NEOPROFILER_REGION_SIZE 0x1000

#define REG_CONTROL   0x00
#define REG_STATUS    0x04
#define REG_DATA      0x08


static dev_t dev;
static struct cdev my_cdev;
struct device *my_device;
struct class *my_class;

static void __iomem *mmio_base;

// Reg read
static inline unsigned int neoprofiler_read(unsigned long long reg_offset)
{
    return readl(mmio_base + reg_offset);
}


int my_open(struct inode *inode, struct file *filp)
{
    printk("Device opened\n");
    return 0;
}

int my_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "Device closed\n");
    return 0;
}


ssize_t my_mmio_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    volatile uint32_t value = neoprofiler_read(REG_CONTROL);
    // char value_str[20];
    // ssize_t ret;
    // snprintf(value_str, sizeof(value_str), "%lu\n", value); // convert to string
    // ret = simple_read_from_buffer(buf, count, f_pos, value_str, strlen(value_str));  

    printk("MMIO Read Value %d \n", value);

    // size_t bytes_to_read = min(count, sizeof(value));

    if (copy_to_user(buf, &value, sizeof(value))) {
        printk("MMIO Read Failed  ");
        return -EFAULT;
    }
    // if (ret >= 0)
    //     *ppos += ret;
    return sizeof(value);
}

ssize_t my_mmio_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    volatile uint32_t *mmio_reg = (uint32_t *)mmio_base;

    volatile uint32_t value;

    if (copy_from_user(&value, buf, sizeof(value)))
        return -EFAULT;

    *mmio_reg = value;

    *f_pos += sizeof(value);

    return sizeof(value);
}



struct file_operations my_mmio_fops = {
    .owner = THIS_MODULE,
    .read = my_mmio_read,
    .write = my_mmio_write,
    .open = my_open,
    .release = my_release
};


// static struct miscdevice my_mmio_dev = {
//     .minor = MISC_DYNAMIC_MINOR,
//     .name = "neoprofiler",
//     .fops = &my_mmio_fops,
// };

// Reg write
static inline void neoprofiler_write(unsigned int reg_offset, unsigned int value)
{
    writel(value, mmio_base + reg_offset);
}

static int __init neoprofiler_init(void)
{
    // Allocate a major number for the device
    if (alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME) < 0) {
        printk("Failed to allocate a major number\n");
        return -1;
    }
    // Initialize the character device structure
    cdev_init(&my_cdev, &my_mmio_fops);
    my_cdev.owner = THIS_MODULE;

    int rc = cdev_add(&my_cdev, dev, 1);
    if (rc < 0) {
        printk("Failed to add cdev\n");
        return rc;
    }
    
    my_class = class_create(THIS_MODULE, "neoprofiler_class");
    if (IS_ERR(my_class)) {
        printk(KERN_ALERT "Failed to create device class\n");
        return PTR_ERR(my_class);
    }

    my_device = device_create(my_class, NULL, dev, NULL, "neoprofiler");
    if (IS_ERR(my_device)) {
        printk(KERN_ALERT "Failed to create device node\n");
        class_destroy(my_class); 
        return PTR_ERR(my_device);
    }

    // Mapping MMIO regs
    mmio_base = ioremap(NEOPROFILER_MMIO_BASE, NEOPROFILER_REGION_SIZE);
    if (!mmio_base) {
        printk("Failed to map MMIO registers\n");
        return -ENOMEM;
    }

    // Read example: read coprocessor status register
    unsigned int status = neoprofiler_read(REG_CONTROL);
    pr_info("Neoprofiler counter: 0x%X\n", status);

    return 0;
}


static void __exit neoprofiler_exit(void)
{
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev, 1);

    if (mmio_base)
        iounmap(mmio_base);
}

module_init(neoprofiler_init);
module_exit(neoprofiler_exit);


MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("PKUZHOU");
MODULE_DESCRIPTION("Neoprofiler Linux driver");
