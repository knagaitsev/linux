#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h> // for find_task_by_vpid, task_struct
#include <linux/spinlock.h>
#include <asm/atomic.h>

#include <linux/ioctl.h>

// IOCTL command to register the calling process for tracking
#define CYCLE_TRACKER_REGISTER_PID 0xBDBD0001

// IOCTL command to clear all recorded data
#define CYCLE_TRACKER_CLEAR_DATA 0xBDBD0002

void record_irq_cycle_time(uint64_t cycles);

#define MODULE_NAME "cycle_tracker"
#define DEVICE_NAME "cycle_tracker"
#define CLASS_NAME  "cycle_tracker_class"

#define MAJOR_NUM 177 // Using a more unique major number
#define MAX_MINORS 1

// Define the size of our cycle time buffer. 1 million uint64_t entries = 8MB.
#define MAX_SAMPLES (1024 * 1024)

// --- Global Variables ---

// The task_struct of the process we are tracking
static struct task_struct *tracked_task = NULL;
// Spinlock to protect access to tracked_task
static DEFINE_SPINLOCK(tracker_lock);

// The large array to store cycle times
static uint64_t *cycle_times;
// Atomic counter for the number of samples recorded
static atomic64_t sample_count;

// Character device structures
static struct cdev my_cdev;
static struct class *my_class;
static dev_t dev_num;

// --- Function to be called from IRQ handler ---

/**
 * record_irq_cycle_time - Record a cycle timestamp if it's for the tracked process.
 * @cycles: The cycle count to record.
 *
 * This function is exported to be called from other parts of the kernel,
 * specifically the GICv3 IRQ handler. It is designed to be extremely fast.
 * It performs a quick check on the 'current' task pointer and uses an
 * atomic increment for the array index to avoid locking in the hot path.
 */
void record_irq_cycle_time(uint64_t cycles)
{
	long idx;

	// Fast path check: no task is being tracked.
	// We read tracked_task without a lock here for speed. A NULL check is safe.
	// The real check is inside the lock in the ioctl.
	if (!tracked_task) {
		return;
	}

	// Check if the currently executing task is the one we want to track
	if (current == tracked_task) {
		// Atomically get the current index and increment it
		idx = atomic64_fetch_add(1, &sample_count);

		// Store the cycle count if we are within the buffer bounds
		if (idx < MAX_SAMPLES) {
			cycle_times[idx] = cycles;
		}
	}
}
EXPORT_SYMBOL_GPL(record_irq_cycle_time);


// --- File Operations ---

static int tracker_open(struct inode *inode, struct file *file)
{
	pr_info("%s: Device opened.\n", MODULE_NAME);
	return 0;
}

static int tracker_release(struct inode *inode, struct file *file)
{
	pr_info("%s: Device closed.\n", MODULE_NAME);
	return 0;
}

static ssize_t tracker_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
	long current_samples;
	size_t total_bytes;
	size_t bytes_to_copy;

	// Get a consistent snapshot of the sample count
	current_samples = atomic64_read(&sample_count);
	if (current_samples > MAX_SAMPLES) {
		current_samples = MAX_SAMPLES;
	}

	pr_info("Tracker sample count: %lu\n", current_samples);

	total_bytes = current_samples * sizeof(uint64_t);

	// Check for end of file
	if (*off >= total_bytes) {
		return 0;
	}

	// Adjust length to not read past the available data
	if (*off + len > total_bytes) {
		len = total_bytes - *off;
	}

	bytes_to_copy = copy_to_user(buf, (char *)cycle_times + *off, len);
	if (bytes_to_copy) {
		pr_warn("%s: Failed to copy %zu bytes to user space\n", MODULE_NAME, bytes_to_copy);
		return -EFAULT;
	}

	// Update the file offset
	*off += len;
	return len;
}

static long tracker_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	struct task_struct *old_task = NULL;

	switch (cmd) {
	case CYCLE_TRACKER_REGISTER_PID:
		spin_lock(&tracker_lock);

		// If a task is already being tracked, release our reference to it.
		if (tracked_task) {
			old_task = tracked_task;
		}

		// Set the new task to be the current one.
		// get_task_struct increments the usage count.
		tracked_task = get_task_struct(current);
		pr_info("%s: Registered PID %d for tracking.\n", MODULE_NAME, current->pid);

		// Reset the sample counter for the new tracking session
		atomic64_set(&sample_count, 0);

		spin_unlock(&tracker_lock);

		// Release the old task outside the lock
		if (old_task) {
			put_task_struct(old_task);
		}
		break;

	case CYCLE_TRACKER_CLEAR_DATA:
		pr_info("%s: Clearing data buffer.\n", MODULE_NAME);
		atomic64_set(&sample_count, 0);
		break;

	default:
		return -ENOTTY; // Command not supported
	}

	return 0;
}

static const struct file_operations fops = {
	.owner          = THIS_MODULE,
	.open           = tracker_open,
	.release        = tracker_release,
	.read           = tracker_read,
	.unlocked_ioctl = tracker_ioctl,
};


// --- Module Init and Exit ---

static int __init cycle_tracker_init(void)
{
	int err;

	// 1. Allocate a major number
	err = alloc_chrdev_region(&dev_num, 0, MAX_MINORS, DEVICE_NAME);
	if (err < 0) {
		pr_err("%s: Failed to allocate major number\n", MODULE_NAME);
		return err;
	}
	pr_info("%s: Major number %d allocated.\n", MODULE_NAME, MAJOR(dev_num));

	// 2. Create the device class
	my_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(my_class)) {
		unregister_chrdev_region(dev_num, MAX_MINORS);
		pr_err("%s: Failed to create device class\n", MODULE_NAME);
		return PTR_ERR(my_class);
	}

	// 3. Create the device
	if (IS_ERR(device_create(my_class, NULL, dev_num, NULL, DEVICE_NAME))) {
		class_destroy(my_class);
		unregister_chrdev_region(dev_num, MAX_MINORS);
		pr_err("%s: Failed to create the device\n", MODULE_NAME);
		return -1;
	}

	// 4. Initialize and add the character device
	cdev_init(&my_cdev, &fops);
	err = cdev_add(&my_cdev, dev_num, 1);
	if (err) {
		device_destroy(my_class, dev_num);
		class_destroy(my_class);
		unregister_chrdev_region(dev_num, MAX_MINORS);
		pr_err("%s: Failed to add cdev\n", MODULE_NAME);
		return err;
	}

	// 5. Allocate memory for cycle times
	cycle_times = kmalloc_array(MAX_SAMPLES, sizeof(uint64_t), GFP_KERNEL);
	if (!cycle_times) {
		cdev_del(&my_cdev);
		device_destroy(my_class, dev_num);
		class_destroy(my_class);
		unregister_chrdev_region(dev_num, MAX_MINORS);
		pr_err("%s: Failed to allocate memory for samples\n", MODULE_NAME);
		return -ENOMEM;
	}

	atomic64_set(&sample_count, 0);

	pr_info("%s: Module loaded successfully.\n", MODULE_NAME);
	return 0;
}

static void __exit cycle_tracker_exit(void)
{
	spin_lock(&tracker_lock);
	if (tracked_task) {
		put_task_struct(tracked_task);
		tracked_task = NULL;
	}
	spin_unlock(&tracker_lock);

	kfree(cycle_times);
	cdev_del(&my_cdev);
	device_destroy(my_class, dev_num);
	class_destroy(my_class);
	unregister_chrdev_region(dev_num, MAX_MINORS);
	pr_info("%s: Module unloaded.\n", MODULE_NAME);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A module to track IRQ entry cycle times for a specific process.");
module_init(cycle_tracker_init);
module_exit(cycle_tracker_exit);
