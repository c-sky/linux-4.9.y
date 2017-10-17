#ifndef __ASM_CSKY_IRQ_H
#define __ASM_CSKY_IRQ_H

#define NR_IRQS CONFIG_CSKY_NR_IRQS

#include <asm-generic/irq.h>

extern unsigned int (*csky_get_auto_irqno) (void);

#endif /* __ASM_CSKY_IRQ_H */
