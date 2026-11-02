/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Userspace ABI for the edu PCI driver (/dev/eduN).
 *
 *   read()/write()   DMA between your buffer and the card's 4 KiB on-card
 *                    buffer; the file position is the offset in that buffer
 *   EDU_IOC_INFO     identification, IRQ mode and counters
 *   EDU_IOC_FACTORIAL  hardware factorial, completion signalled by interrupt
 *   EDU_IOC_LIVENESS   card liveness check (the card returns ~value)
 */
#ifndef _UAPI_EDU_IOCTL_H
#define _UAPI_EDU_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define EDU_BUF_SIZE 4096

struct edu_info {
	__u32 version;          /* ID register, 0xRRrr00ed               */
	__u32 irq_is_msi;       /* 1 = MSI, 0 = legacy INTx              */
	__u64 irq_count;        /* interrupts handled                    */
	__u64 spurious_irqs;    /* IRQs with empty status (shared line)  */
	__u64 dma_count;        /* DMA transfers completed               */
	__u64 dma_bytes;        /* bytes moved by DMA                    */
	__u64 timeouts;         /* operations that never got their IRQ   */
};

struct edu_factorial {
	__u32 n;                /* in                                    */
	__u32 result;           /* out (32-bit, wraps like the hardware) */
};

#define EDU_IOC_MAGIC     'E'
#define EDU_IOC_INFO      _IOR(EDU_IOC_MAGIC, 1, struct edu_info)
#define EDU_IOC_FACTORIAL _IOWR(EDU_IOC_MAGIC, 2, struct edu_factorial)
#define EDU_IOC_LIVENESS  _IOWR(EDU_IOC_MAGIC, 3, __u32)

#endif /* _UAPI_EDU_IOCTL_H */
