#ifndef DEFS_H
#define DEFS_H

typedef unsigned long int size_t;

static inline void *
memcpy(void *dst, const void *src, size_t count)
{
	const u8_t *s = (const u8_t *)src;
	u8_t *      d = (u8_t *)dst;

	for (; count != 0; count--) *d++ = *s++;

	return dst;
}

static inline void *
memset(void *dst, int c, size_t count)
{
	char *p = (char *)dst;

	for (; count != 0; count--) *p++ = c;

	return dst;
}

#endif /* DEFS_H */
