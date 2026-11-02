// SPDX-License-Identifier: GPL-2.0
/*
 * edu.c - Linux PCI driver for QEMU's "edu" educational device (1234:11e8).
 *
 * Covers the pieces every real PCI driver needs:
 *   - PCI probe/remove with managed (devm/pcim) resources, so every error
 *     path and unbind releases exactly what was acquired
 *   - BAR0 MMIO register access
 *   - MSI interrupts with automatic fallback to shared legacy INTx
 *   - DMA with a correctly declared 28-bit DMA mask and a coherent buffer
 *   - a misc char device (/dev/eduN) exposing read/write (DMA) and ioctls
 *   - completion-based waits with timeouts, so a dead device returns
 *     -ETIMEDOUT instead of hanging the calling process forever
 *   - safe hot-unplug: the device struct is reference counted, so a process
 *     still holding /dev/eduN open after the card is removed gets -ENODEV
 *     instead of touching freed memory or unmapped registers
 *
 * Every DMA range is validated in the driver before it is programmed: the
 * device has no IOMMU-style protection of its own and an out-of-range
 * request takes the whole machine (QEMU) down.
 */
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "../include/uapi/edu_ioctl.h"

#define DRV_NAME          "edu"
#define EDU_VENDOR_ID     0x1234
#define EDU_DEVICE_ID     0x11e8

/* Register map (docs/specs/edu.rst in the QEMU tree). */
#define EDU_REG_ID        0x00
#define EDU_REG_LIVENESS  0x04
#define EDU_REG_FACT      0x08
#define EDU_REG_STATUS    0x20
#define   EDU_STATUS_COMPUTING  0x01
#define   EDU_STATUS_IRQ_FACT   0x80
#define EDU_REG_IRQ_STATUS 0x24
#define EDU_REG_IRQ_RAISE  0x60
#define EDU_REG_IRQ_ACK    0x64
#define EDU_REG_DMA_SRC    0x80
#define EDU_REG_DMA_DST    0x88
#define EDU_REG_DMA_CNT    0x90
#define EDU_REG_DMA_CMD    0x98
#define   EDU_DMA_RUN           0x01
#define   EDU_DMA_TO_RAM        0x02      /* direction: card -> RAM */
#define   EDU_DMA_IRQ           0x04

#define EDU_IRQ_FACT      0x00000001
#define EDU_IRQ_DMA       0x00000100

#define EDU_DEV_BUF_ADDR  0x40000         /* on-card buffer, device address */
#define EDU_DMA_BITS      28
#define EDU_TIMEOUT       msecs_to_jiffies(2000)

struct edu_dev {
	struct kref          ref;         /* probe + one per open file      */
	bool                 gone;        /* device removed; under lock     */
	struct pci_dev      *pdev;
	void __iomem        *mmio;
	struct miscdevice    misc;
	char                 name[16];
	int                  id;

	struct mutex         lock;        /* one DMA / factorial at a time */
	struct completion    fact_done;
	struct completion    dma_done;

	void                *dma_buf;     /* coherent, CPU side */
	dma_addr_t           dma_handle;  /* bus address for the card */

	bool                 msi;
	atomic64_t           irq_count;
	atomic64_t           spurious_irqs;
	atomic64_t           dma_count;
	atomic64_t           dma_bytes;
	atomic64_t           timeouts;
};

static DEFINE_IDA(edu_ida);

static void edu_kref_release(struct kref *ref)
{
	kfree(container_of(ref, struct edu_dev, ref));
}

/* Take the lock and fail if the card has been unplugged. Every path that
 * touches registers or the DMA buffer goes through here. */
static int edu_lock_live(struct edu_dev *edu)
{
	if (mutex_lock_interruptible(&edu->lock))
		return -ERESTARTSYS;
	if (edu->gone) {
		mutex_unlock(&edu->lock);
		return -ENODEV;
	}
	return 0;
}

static inline u32 edu_rd32(struct edu_dev *edu, u32 reg)
{
	return ioread32(edu->mmio + reg);
}

static inline void edu_wr32(struct edu_dev *edu, u32 reg, u32 val)
{
	iowrite32(val, edu->mmio + reg);
}

/* 64-bit DMA registers: 8-byte access is allowed at >= 0x80. Two 32-bit
 * writes are used for portability to platforms without writeq. */
static inline void edu_wr64(struct edu_dev *edu, u32 reg, u64 val)
{
	iowrite32(lower_32_bits(val), edu->mmio + reg);
	iowrite32(upper_32_bits(val), edu->mmio + reg + 4);
}

/* ------------------------------------------------------------------ IRQ */
static irqreturn_t edu_isr(int irq, void *data)
{
	struct edu_dev *edu = data;
	u32 status = edu_rd32(edu, EDU_REG_IRQ_STATUS);

	if (!status) {
		/* Legacy INTx can be shared: not ours. */
		atomic64_inc(&edu->spurious_irqs);
		return IRQ_NONE;
	}

	/* Ack first so a new event raised while we run is not lost. Needed
	 * for MSI too: the device keeps the status latched until acked. */
	edu_wr32(edu, EDU_REG_IRQ_ACK, status);
	atomic64_inc(&edu->irq_count);

	if (status & EDU_IRQ_FACT)
		complete(&edu->fact_done);
	if (status & EDU_IRQ_DMA)
		complete(&edu->dma_done);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ DMA */

/* Lock held. dir_to_ram: card buffer -> RAM. */
static int edu_dma(struct edu_dev *edu, u32 dev_off, size_t len, bool dir_to_ram)
{
	u64 card = EDU_DEV_BUF_ADDR + dev_off;

	/* Validate before touching the hardware (see file header). */
	if (len == 0 || len > EDU_BUF_SIZE || dev_off > EDU_BUF_SIZE - len)
		return -EINVAL;

	reinit_completion(&edu->dma_done);
	if (dir_to_ram) {
		edu_wr64(edu, EDU_REG_DMA_SRC, card);
		edu_wr64(edu, EDU_REG_DMA_DST, edu->dma_handle);
	} else {
		edu_wr64(edu, EDU_REG_DMA_SRC, edu->dma_handle);
		edu_wr64(edu, EDU_REG_DMA_DST, card);
	}
	edu_wr64(edu, EDU_REG_DMA_CNT, len);
	/* iowrite32 orders the CPU's writes to dma_buf before the doorbell. */
	edu_wr32(edu, EDU_REG_DMA_CMD,
		 EDU_DMA_RUN | EDU_DMA_IRQ | (dir_to_ram ? EDU_DMA_TO_RAM : 0));

	if (!wait_for_completion_timeout(&edu->dma_done, EDU_TIMEOUT)) {
		atomic64_inc(&edu->timeouts);
		dev_err(&edu->pdev->dev, "DMA %s timed out (off=%u len=%zu)\n",
			dir_to_ram ? "card->RAM" : "RAM->card", dev_off, len);
		return -ETIMEDOUT;
	}
	dma_rmb();      /* see the device's writes to dma_buf after the IRQ */

	atomic64_inc(&edu->dma_count);
	atomic64_add(len, &edu->dma_bytes);
	return 0;
}

/* ------------------------------------------------------------ char dev */
static struct edu_dev *file_edu(struct file *f)
{
	return container_of(f->private_data, struct edu_dev, misc);
}

static ssize_t edu_read(struct file *f, char __user *ubuf, size_t len,
			loff_t *ppos)
{
	struct edu_dev *edu = file_edu(f);
	ssize_t ret;

	if (*ppos < 0)
		return -EINVAL;
	if (*ppos >= EDU_BUF_SIZE)
		return 0;                               /* EOF */
	len = min_t(size_t, len, EDU_BUF_SIZE - *ppos);
	if (!len)
		return 0;

	ret = edu_lock_live(edu);
	if (ret)
		return ret;
	ret = edu_dma(edu, (u32)*ppos, len, true);
	if (!ret) {
		if (copy_to_user(ubuf, edu->dma_buf, len)) {
			ret = -EFAULT;
		} else {
			*ppos += len;
			ret = len;
		}
	}
	mutex_unlock(&edu->lock);
	return ret;
}

static ssize_t edu_write(struct file *f, const char __user *ubuf, size_t len,
			 loff_t *ppos)
{
	struct edu_dev *edu = file_edu(f);
	ssize_t ret;

	if (*ppos < 0)
		return -EINVAL;
	if (*ppos >= EDU_BUF_SIZE)
		return len ? -ENOSPC : 0;
	len = min_t(size_t, len, EDU_BUF_SIZE - *ppos);
	if (!len)
		return 0;

	ret = edu_lock_live(edu);
	if (ret)
		return ret;
	if (copy_from_user(edu->dma_buf, ubuf, len)) {
		ret = -EFAULT;
	} else {
		ret = edu_dma(edu, (u32)*ppos, len, false);
		if (!ret) {
			*ppos += len;
			ret = len;
		}
	}
	mutex_unlock(&edu->lock);
	return ret;
}

static loff_t edu_llseek(struct file *f, loff_t off, int whence)
{
	return fixed_size_llseek(f, off, whence, EDU_BUF_SIZE);
}

static long edu_factorial(struct edu_dev *edu, struct edu_factorial *fa)
{
	long ret = edu_lock_live(edu);

	if (ret)
		return ret;

	reinit_completion(&edu->fact_done);
	edu_wr32(edu, EDU_REG_STATUS, EDU_STATUS_IRQ_FACT); /* IRQ on finish */
	edu_wr32(edu, EDU_REG_FACT, fa->n);                 /* starts it     */

	if (!wait_for_completion_timeout(&edu->fact_done, EDU_TIMEOUT)) {
		atomic64_inc(&edu->timeouts);
		dev_err(&edu->pdev->dev, "factorial(%u) timed out\n", fa->n);
		ret = -ETIMEDOUT;
	} else {
		fa->result = edu_rd32(edu, EDU_REG_FACT);
	}
	mutex_unlock(&edu->lock);
	return ret;
}

static long edu_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct edu_dev *edu = file_edu(f);
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case EDU_IOC_INFO: {
		struct edu_info info = {
			.irq_is_msi    = edu->msi,
			.irq_count     = atomic64_read(&edu->irq_count),
			.spurious_irqs = atomic64_read(&edu->spurious_irqs),
			.dma_count     = atomic64_read(&edu->dma_count),
			.dma_bytes     = atomic64_read(&edu->dma_bytes),
			.timeouts      = atomic64_read(&edu->timeouts),
		};
		int ret = edu_lock_live(edu);

		if (ret)
			return ret;
		info.version = edu_rd32(edu, EDU_REG_ID);
		mutex_unlock(&edu->lock);
		return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case EDU_IOC_FACTORIAL: {
		struct edu_factorial fa;
		long ret;

		if (copy_from_user(&fa, uarg, sizeof(fa)))
			return -EFAULT;
		ret = edu_factorial(edu, &fa);
		if (ret)
			return ret;
		return copy_to_user(uarg, &fa, sizeof(fa)) ? -EFAULT : 0;
	}
	case EDU_IOC_LIVENESS: {
		u32 v;

		int ret;

		if (get_user(v, (u32 __user *)uarg))
			return -EFAULT;
		ret = edu_lock_live(edu);
		if (ret)
			return ret;
		edu_wr32(edu, EDU_REG_LIVENESS, v);
		v = edu_rd32(edu, EDU_REG_LIVENESS);
		mutex_unlock(&edu->lock);
		return put_user(v, (u32 __user *)uarg);
	}
	default:
		return -ENOTTY;
	}
}

/* misc_open() holds misc_mtx while calling us, and misc_deregister() takes
 * the same mutex, so the device cannot be freed between lookup and kref_get. */
static int edu_open(struct inode *inode, struct file *f)
{
	kref_get(&file_edu(f)->ref);
	return 0;
}

static int edu_release(struct inode *inode, struct file *f)
{
	kref_put(&file_edu(f)->ref, edu_kref_release);
	return 0;
}

static const struct file_operations edu_fops = {
	.owner          = THIS_MODULE,
	.open           = edu_open,
	.release        = edu_release,
	.read           = edu_read,
	.write          = edu_write,
	.llseek         = edu_llseek,
	.unlocked_ioctl = edu_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* ---------------------------------------------------------- probe/remove */
static void edu_free_irq_vectors(void *data)
{
	pci_free_irq_vectors(data);
}

static void edu_put(void *data)
{
	struct edu_dev *edu = data;

	kref_put(&edu->ref, edu_kref_release);
}

static void edu_free_id(void *data)
{
	ida_free(&edu_ida, (int)(long)data);
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct edu_dev *edu;
	u32 ver, probe_val = 0x5a5aa5a5;
	int ret, irq;

	/* Not devm: open files may outlive the device. The probe's reference
	 * is dropped by devm after remove(), the rest by edu_release(). */
	edu = kzalloc(sizeof(*edu), GFP_KERNEL);
	if (!edu)
		return -ENOMEM;
	kref_init(&edu->ref);
	ret = devm_add_action_or_reset(dev, edu_put, edu);
	if (ret)
		return ret;
	edu->pdev = pdev;
	mutex_init(&edu->lock);
	init_completion(&edu->fact_done);
	init_completion(&edu->dma_done);
	pci_set_drvdata(pdev, edu);

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "enable failed\n");

	ret = pcim_iomap_regions(pdev, BIT(0), DRV_NAME);
	if (ret)
		return dev_err_probe(dev, ret, "BAR0 map failed\n");
	edu->mmio = pcim_iomap_table(pdev)[0];

	/* Sanity-check we are really talking to an edu card. */
	ver = edu_rd32(edu, EDU_REG_ID);
	if ((ver & 0xff) != 0xed)
		return dev_err_probe(dev, -ENODEV, "unexpected ID 0x%08x\n", ver);
	edu_wr32(edu, EDU_REG_LIVENESS, probe_val);
	if (edu_rd32(edu, EDU_REG_LIVENESS) != ~probe_val)
		return dev_err_probe(dev, -EIO, "liveness check failed\n");

	/* The card only drives 28 address bits; tell the DMA layer so it
	 * never hands us a buffer above 256 MiB. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(EDU_DMA_BITS));
	if (ret)
		return dev_err_probe(dev, ret, "no usable %d-bit DMA\n", EDU_DMA_BITS);
	edu->dma_buf = dmam_alloc_coherent(dev, EDU_BUF_SIZE, &edu->dma_handle,
					   GFP_KERNEL);
	if (!edu->dma_buf)
		return -ENOMEM;
	pci_set_master(pdev);

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_LEGACY);
	if (ret < 0)
		return dev_err_probe(dev, ret, "no IRQ vector\n");
	ret = devm_add_action_or_reset(dev, edu_free_irq_vectors, pdev);
	if (ret)
		return ret;
	edu->msi = pdev->msi_enabled;
	irq = pci_irq_vector(pdev, 0);
	ret = devm_request_irq(dev, irq, edu_isr, edu->msi ? 0 : IRQF_SHARED,
			       DRV_NAME, edu);
	if (ret)
		return dev_err_probe(dev, ret, "request_irq(%d) failed\n", irq);

	edu->id = ida_alloc(&edu_ida, GFP_KERNEL);
	if (edu->id < 0)
		return edu->id;
	ret = devm_add_action_or_reset(dev, edu_free_id, (void *)(long)edu->id);
	if (ret)
		return ret;

	snprintf(edu->name, sizeof(edu->name), DRV_NAME "%d", edu->id);
	edu->misc.minor  = MISC_DYNAMIC_MINOR;
	edu->misc.name   = edu->name;
	edu->misc.fops   = &edu_fops;
	edu->misc.parent = dev;
	edu->misc.mode   = 0600;
	ret = misc_register(&edu->misc);
	if (ret)
		return dev_err_probe(dev, ret, "misc_register failed\n");

	dev_info(dev, "edu v%u.%u at /dev/%s, %s irq %d, dma buf %pad\n",
		 ver >> 24, (ver >> 16) & 0xff, edu->name,
		 edu->msi ? "MSI" : "INTx", irq, &edu->dma_handle);
	return 0;
}

static void edu_remove(struct pci_dev *pdev)
{
	struct edu_dev *edu = pci_get_drvdata(pdev);

	/* Order: remove the user-facing node, mark gone, then devm frees the
	 * IRQ before the DMA buffer and MMIO mapping (reverse of acquisition). */
	misc_deregister(&edu->misc);            /* no new opens after this  */

	/* Wait for any in-flight operation, then fence off open files. */
	mutex_lock(&edu->lock);
	edu->gone = true;
	mutex_unlock(&edu->lock);

	dev_info(&pdev->dev, "removed /dev/%s (irqs=%lld dma=%lld timeouts=%lld)\n",
		 edu->name, atomic64_read(&edu->irq_count),
		 atomic64_read(&edu->dma_count), atomic64_read(&edu->timeouts));
}

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static struct pci_driver edu_driver = {
	.name     = DRV_NAME,
	.id_table = edu_ids,
	.probe    = edu_probe,
	.remove   = edu_remove,
};
module_pci_driver(edu_driver);

MODULE_AUTHOR("Ribhu Mukherjee");
MODULE_DESCRIPTION("PCI driver for the QEMU edu device: MMIO, MSI, DMA, char dev");
MODULE_LICENSE("GPL");
