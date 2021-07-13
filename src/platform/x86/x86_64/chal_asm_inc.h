/* Segment selectors for the GDT */
#define SEL_RPL_USR 0x3

#define SEL_NULL 0x00
#define SEL_KCSEG 0x08                 /* Kernel code selector. */
#define SEL_KDSEG 0x10                 /* Kernel data selector. */
#define SEL_UCSEG (0x20 | SEL_RPL_USR) /* User code selector. */
#define SEL_UDSEG (0x18 | SEL_RPL_USR) /* User data selector. */
#define SEL_TSS 0x28                   /* Task-state segment. */
#define SEL_UGSEG (0x38 | SEL_RPL_USR) /* User TLS selector. */
#define SEL_CNT 8                      /* Number of segments. */

#define STK_INFO_SZ 144                 /* sizeof(struct cos_cpu_local_info) */
#define STK_INFO_OFF (STK_INFO_SZ + 8) /* sizeof(struct cos_cpu_local_info) + sizeof(long) */

#define SMP_BOOT_PATCH_ADDR 0x70000