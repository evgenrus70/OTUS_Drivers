#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

#define MEM_SIZE 1024
#define STACK_MAX_SIZE 1024  // Define a maximum size for stack

dev_t dev = 0;
static struct cdev new_cdev;
static struct class* dev_class;

static int32_t* stack_entries = NULL;
static uint8_t stack_size = 0;
static uint8_t top = 0;  // Index of top element in stack

// Mutex for synchronization
static DEFINE_MUTEX(stack_mutex);

static int new_open(struct inode* inode, struct file* file);
static int new_release(struct inode* inode, struct file* file);
static ssize_t new_read(struct file* file, char __user* buf, size_t len, loff_t* off);
static ssize_t new_write(struct file* file, const char __user* buf, size_t len, loff_t* off);
static long new_ioctl(struct file* file, unsigned int cmd, unsigned long arg);

static struct file_operations f_ops = {
    .owner = THIS_MODULE,
    .read = new_read,
    .write = new_write,
    .open = new_open,
    .release = new_release,
    .unlocked_ioctl = new_ioctl,  // Use unlocked_ioctl
};

static int new_open(struct inode* inode, struct file* file) {
    mutex_lock(&stack_mutex);

    // Initialize stack if not already initialized
    if (stack_entries == NULL) {
        stack_entries = kmalloc(MEM_SIZE * sizeof(int32_t), GFP_KERNEL);
        if (!stack_entries) {
            printk(KERN_ERR "-- Cannot allocate memory for stack.\n");
            mutex_unlock(&stack_mutex);
            return -ENOMEM;
        }
        stack_size = MEM_SIZE;
        top = 0;  // Stack is empty initially
    }
    
    printk(KERN_INFO "-- Device file open.\n");
    mutex_unlock(&stack_mutex);
    return 0;
}

static int new_release(struct inode* inode, struct file* file) {
    mutex_lock(&stack_mutex);

    // Free stack memory on close
    if (stack_entries) {
        kfree(stack_entries);
        stack_entries = NULL;
        stack_size = 0;
        top = 0;
    }

    printk(KERN_INFO "-- Device file closed.\n");
    mutex_unlock(&stack_mutex);
    return 0;
}

static ssize_t new_read(struct file* file, char __user* buf, size_t len, loff_t* off) {
    ssize_t result = 0;
    
    mutex_lock(&stack_mutex);
    
    // Check if the stack is empty
    if (top == 0) {
        mutex_unlock(&stack_mutex);
        printk(KERN_ERR "-- Stack is empty. Cannot pop.\n");
        return -EINVAL;  // Return error if stack is empty
    }

    // Pop the top element from the stack
    result = sizeof(int32_t);
    if (copy_to_user(buf, &stack_entries[top - 1], sizeof(int32_t))) {
        mutex_unlock(&stack_mutex);
        printk(KERN_ERR "-- Failed to copy data to user space.\n");
        return -EFAULT;
    }

    // Decrement the stack top after pop
    top--;
    
    printk(KERN_INFO "-- Data read (popped from stack).\n");
    mutex_unlock(&stack_mutex);
    return result;
}

static ssize_t new_write(struct file* file, const char __user* buf, size_t len, loff_t* off) {
    ssize_t result = 0;

    mutex_lock(&stack_mutex);

    // Check if there's space to push data onto the stack
    if (top >= stack_size) {
        mutex_unlock(&stack_mutex);
        printk(KERN_ERR "-- Stack is full. Cannot push more data.\n");
        return -ENOMEM;  // Return error if stack is full
    }

    // Copy data from user space to the stack
    if (copy_from_user(&stack_entries[top], buf, sizeof(int32_t))) {
        mutex_unlock(&stack_mutex);
        printk(KERN_ERR "-- Failed to copy data from user space.\n");
        return -EFAULT;
    }

    // Increment the stack top after push
    top++;
    
    printk(KERN_INFO "-- Data written (pushed to stack).\n");
    result = sizeof(int32_t);
    mutex_unlock(&stack_mutex);
    return result;
}

static long new_ioctl(struct file* file, unsigned int cmd, unsigned long arg) {
    long result = 0;
    
    mutex_lock(&stack_mutex);
    
    switch (cmd) {
        case 1:  // Resize stack command
            if (arg > 0 && arg <= STACK_MAX_SIZE) {
                // Resize the stack
                int32_t* new_stack = kmalloc(arg * sizeof(int32_t), GFP_KERNEL);
                if (!new_stack) {
                    mutex_unlock(&stack_mutex);
                    printk(KERN_ERR "-- Failed to allocate memory for resizing stack.\n");
                    return -ENOMEM;
                }

                // Copy old stack data to the new one
                if (stack_entries) {
                    memcpy(new_stack, stack_entries, top * sizeof(int32_t));
                    kfree(stack_entries);  // Free old stack memory
                }

                // Update the stack properties
                stack_entries = new_stack;
                stack_size = arg;

                printk(KERN_INFO "-- Stack resized to %lu.\n", arg);
            } else {
                mutex_unlock(&stack_mutex);
                printk(KERN_ERR "-- Invalid stack size.\n");
                return -EINVAL;
            }
            break;

        default:
            mutex_unlock(&stack_mutex);
            printk(KERN_ERR "-- Invalid ioctl command.\n");
            return -EINVAL;
    }

    mutex_unlock(&stack_mutex);
    return result;
}

static int __init character_driver_init(void) {
    if (alloc_chrdev_region(&dev, 0, 1, "new_device") < 0) {
        printk(KERN_ERR "-- Unable to allocate major number.\n");
        return -1;
    }

    printk(KERN_INFO "-- Major = %d Minor = %d\n", MAJOR(dev), MINOR(dev));

    cdev_init(&new_cdev, &f_ops);
    
    if (cdev_add(&new_cdev, dev, 1) < 0) {
        printk(KERN_ERR "-- Cannot add device to the system.\n");
        unregister_chrdev_region(dev, 1);
        return -1;
    }

    if ((dev_class = class_create(THIS_MODULE, "new_class")) == NULL) {
        printk(KERN_ERR "-- Cannot create class.\n");
        cdev_del(&new_cdev);
        unregister_chrdev_region(dev, 1);
        return -1;
    }

    if (device_create(dev_class, NULL, dev, NULL, "new_device") == NULL) {
        printk(KERN_ERR "-- Cannot create device.\n");
        class_destroy(dev_class);
        cdev_del(&new_cdev);
        unregister_chrdev_region(dev, 1);
        return -1;
    }

    printk(KERN_INFO "-- Device created successfully.\n");
    return 0;
}

static void __exit character_driver_exit(void) {
    mutex_lock(&stack_mutex);

    if (stack_entries) {
        kfree(stack_entries);
        stack_entries = NULL;
        stack_size = 0;
        top = 0;
    }

    mutex_unlock(&stack_mutex);

    device_destroy(dev_class, dev);
    class_destroy(dev_class);
    cdev_del(&new_cdev);

    unregister_chrdev_region(dev, 1);

    printk(KERN_INFO "-- Device removed successfully.\n");
}

module_init(character_driver_init);
module_exit(character_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Evgen");
MODULE_DESCRIPTION("NEVER USE THIS MODULE IN SANE MIND.");
