/**
 * Interrupt guard helpers: centralized save/restore of EA
 */
#ifndef IRQ_GUARD_H
#define IRQ_GUARD_H

unsigned char IRQ_Save(void);
void IRQ_Restore(unsigned char prev);

#endif

