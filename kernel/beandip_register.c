#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/sched/task_stack.h>
#include <linux/io.h>

// #include <linux/sched.h>

#include <asm/io.h>
#include <asm/fixmap.h>
#include <asm/smp.h>
// #include <asm/apicdef.h>

#include <asm-generic/fixmap.h>

#include <uapi/asm-generic/errno-base.h>

#define MAJOR_NUM 17
#define MAX_MINORS 1

#define APIC_DEV_CLASS_MODE 0444

// struct beandip_hit_params {
//     int hartid;
//     int hwirq;
// };

static struct cdev apic_cdev;
static struct class *apic_class;

static int apic_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int apic_release(struct inode *inode, struct file *file)
{
	return 0;
}

static char* apic_devnode(struct device *dev, umode_t *mode)
{
	if (mode) {
		*mode = APIC_DEV_CLASS_MODE;
	}
	return NULL;
}

static long apic_ioctl(struct file * fp, unsigned int cmd, unsigned long arg) {
	// struct beandip_hit_params params;

	// switch(cmd) {
	// 	case MY_APIC_PF_INDICATOR:
	// 		printk("Registering page fault indicator\n");
	// 		// current->user_page_fault_indicator = (void __user *) arg;
	// 		// current->beandipped = true;
	// 		break;
	// 	case MY_APIC_SYSCALL_INDICATOR:
	// 		printk("Registering syscall indicator\n");
	// 		// current->user_syscall_indicator = (void __user *) arg;
	// 		// current->beandipped = true;
	// 		break;
	// 	case MY_APIC_HIT_INDICATOR:
	// 		if (copy_from_user(&params, (struct beandip_hit_params *)arg, sizeof(struct beandip_hit_params))) {
    //             return -EFAULT;
    //         }

	// 		int cpuid = riscv_hartid_to_cpuid(params.hartid);

	// 		printk(KERN_INFO "Received poll hit ioctl with hartid: %d, hwirq: %d, cpuid: %d\n", params.hartid, params.hwirq, cpuid);

	// 		plic_irq_claim_handle_cpu_hwirq(cpuid, params.hwirq);

	// 		// current->user_syscall_indicator = (void __user *) arg;
	// 		// current->beandipped = true;
	// 		break;
	// 	default:
	// 		printk("Hit default ioctl case\n");
	// 		break;
	// }
    struct task_struct *tsk;

    tsk = current;

    printk("ioctl pid: %d\n", tsk->pid);

	return 0;
}

static struct file_operations fops = 
{
	.owner			= THIS_MODULE,
	.open			= apic_open,
	.unlocked_ioctl = apic_ioctl,
	.release		= apic_release
};

int setup_chrdev(void) {
	int err;
	err = register_chrdev_region(MKDEV(MAJOR_NUM, 0), MAX_MINORS, "apic_device_driver");

	if (err != 0){
		printk("apic: failed to register chrdev\n");
		return err;
	}

	cdev_init(&apic_cdev, &fops);
	// Linux 6.4
	// apic_class = class_create("apic_class");
	apic_class = class_create(THIS_MODULE, "apic_class");
	apic_class->devnode = apic_devnode;
	device_create(apic_class, NULL, MKDEV(MAJOR_NUM, 0), NULL, "apic_dev");
	cdev_add(&apic_cdev, MKDEV(MAJOR_NUM, 0), 1);

	return 0;
}

int teardown_chrdev(void) {
	device_destroy(apic_class, MKDEV(MAJOR_NUM, 0));
	class_destroy(apic_class);
	cdev_del(&apic_cdev);
	unregister_chrdev_region(MKDEV(MAJOR_NUM, 0), MAX_MINORS);
	printk("Unregistered beandip_userspace\n");
	return 0;
}

static int __init beandip_register_init(void)
{
	int err;

	// setup /dev/apic_dev
	err = setup_chrdev();
	if (err) {
		printk("apic_dev_kmod: failed to register device!\n");
		return err;
	}

	return 0;
}


static void __exit beandip_register_exit(void)
{
	/* unregister the device */
	teardown_chrdev();
}

MODULE_LICENSE("GPL");
module_init(beandip_register_init);
module_exit(beandip_register_exit);
