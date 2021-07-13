#ifndef COS_IO_H
#define COS_IO_H

/**
 * Write byte to specific port
 */
static inline void
outb(u16_t port, u8_t value)
{
	__asm__ __volatile__("outb %1, %0" : : "dN"(port), "a"(value));
}

/**
 * Read byte from port
 */
static inline u8_t
inb(u16_t port)
{
	u8_t ret;

	__asm__ __volatile__("inb %1, %0" : "=a"(ret) : "dN"(port));

	return ret;
}

#endif /* COS_IO_H */
