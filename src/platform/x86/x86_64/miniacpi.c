#include "kernel.h"
#include "string.h"
#include "mem_layout.h"
#include "pgtbl.h"

//#define RSDP_LO_ADDRESS ((unsigned char *)0xc00E0000)
//#define RSDP_HI_ADDRESS ((unsigned char *)0xc00FFFFF)
#define RSDP_LO_ADDRESS ((unsigned char *)(0x000E0000+COS_MEM_KERN_START_VA))
#define RSDP_HI_ADDRESS ((unsigned char *)(0x000FFFFF+COS_MEM_KERN_START_VA))
#define RSDP_ALIGNMENT (16)

struct rsdp {
	char  signature[8];
	u8_t  checksum;
	char  oemid[6];
	u8_t  revision;
	u32_t rsdtaddress;
	u32_t length;
	u64_t xsdtaddress;
	u8_t  extendedchecksum;
	u8_t  reserved[3];
} __attribute__((packed));

struct acpi_header {
		char  sig[4];
		u32_t len;
};

struct rsdt {
	struct acpi_header head;
	u8_t         revision;
	u8_t         checksum;
	char         oemid[6];
	char         oemtableid[8];
	u32_t        oemrevision;
	u32_t        creatorid;
	u32_t        creatorrevision;
	/* 
	 * have to be a u32_t size for entry because paddr_t in 64-bit system is 8 bytes 
	 * but rsdt make sense only when address is 32-bit
	*/
	u32_t      entry[0];
} __attribute__((packed));

static struct rsdt *rsdt;

void *
acpi_find_rsdt(void)
{
	unsigned char *sig;
	struct rsdp   *rsdp = NULL;
	paddr_t        rsdt_pa;

	for (sig = RSDP_LO_ADDRESS; sig < RSDP_HI_ADDRESS; sig += RSDP_ALIGNMENT) {
		if (!strncmp("RSD PTR ", (char *)sig, 8)) {
			struct rsdp * r   = (struct rsdp *)sig;
			unsigned char sum = 0;
			u32_t         i;

			for (i = 0; i < r->length; i++) {
				sum += sig[i];
			}

			if (sum == 0) {
				printk("\tFound good RSDP @ %p\n", sig);
				rsdp = (struct rsdp *)sig;
				break;
			} else {
				printk("\tFound RSDP signature but bad checksum (%d) @ %p\n", sum, sig);
			}
		}
	}

	if (!rsdp) return NULL;
	printk("\tRDST paddr is @ %p\n", rsdp->rsdtaddress);
	printk("\tRSDP lenth is :%u\n", rsdp->length);
	printk("\tXSDT paddr is @ %p\n", rsdp->xsdtaddress);
	rsdt_pa = (paddr_t)rsdp->rsdtaddress;
	return device_map_mem(rsdt_pa, 0);
}

static int
acpi_chk_header(void *ptr, const char *sig)
{
	struct acpi_header *h = ptr;
	char *mem;
	char  sum = 0;
	u32_t i;

	if (strncmp(h->sig, sig, 4)) return -1;
	mem = ptr;

	for (i = 0; i < h->len; i++) {
		sum += mem[i];
	}
	if (sum != 0) {
		printk("\tChecksum of acpi resource failed (with chksum %d)\n", sum % 255);
		return -1;
	}

	return 0;
}


static void *
acpi_find_resource_flags(const char *res_name, pgtbl_flags_t flags)
{
	size_t  i;
	size_t entries = (rsdt->head.len - sizeof(struct rsdt)) / 4;
	for (i = 0; i < entries; i++) {
		struct rsdt *e = (struct rsdt *)device_pa2va(rsdt->entry[i]);

		if (!e) {
			/*
			 * I don't think this should commonly happen
			 * as the resources should be on the same
			 * super-page as the parent rsdt.
			 */
			e = device_map_mem((u32_t)rsdt->entry[i], flags);
			assert(e);
		}
		if (!acpi_chk_header(e, res_name)) return e;
	}

	return NULL;
}

static void *
acpi_find_resource(const char *res_name)
{
	return acpi_find_resource_flags(res_name, 0);
}

void *
acpi_find_timer(void)
{
	return acpi_find_resource("HPET");
}

void *
acpi_find_apic(void)
{
	return acpi_find_resource_flags("APIC", PGTBL_NOCACHE);
}

/*
 * Thanks to kaworu @ https://forum.osdev.org/viewtopic.php?t=16990
 * for shutdown code. For this structures layout, see 5.2.9 Fixed ACPI
 * Description Table (FADT) of the ACPI spec.
 */
struct facp {
	struct acpi_header head;
	u8_t   _unneeded1[40 - 8]; /* see offsets in spec */
	u32_t dsdt;
	char   _unneeded2[48 - 44];
	u32_t smi_cmd;
	u8_t   acpi_enable;
	u8_t   acpi_disable;
	char   unneeded3[64 - 54];
	u32_t  pm1a_cnt_blk;
	u32_t  pm1b_cnt_blk;
	u8_t   unneeded4[89 - 72];
	u8_t   pm1_cnt_len;
} __attribute__((packed));

#define DSDT_DEFBLK_OFFSET 36  /* offset to the "Definition Block" */

void
outw(unsigned short __val, unsigned short __port)
{
	__asm__ volatile("outw %0,%1" : : "a" (__val), "dN" (__port));
}

static void
outb(unsigned char __val, unsigned short __port)
{
	__asm__ volatile("outb %0,%1" : : "a" (__val), "dN" (__port));
}

static unsigned short
inw(unsigned short __port)
{
	unsigned short __val;

	__asm__ volatile("inw %1,%0" : "=a" (__val) : "dN" (__port));

	return __val;
}

static u32_t  pm1a_cnt;
static u32_t  pm1b_cnt;
static u16_t  slp_type_a;
static u16_t  slp_type_b;
static u16_t  slp_en;
static u32_t  smi_cmd;
static u8_t   acpi_enable;
static int    shutdown_ready;

void
acpi_shutdown_init(void)
{
	paddr_t facp_pa, dsdt_pa;
	struct facp *facp = NULL;
	struct acpi_header *dsdt = NULL;

	char  *s5_addr;
	int    dsdt_len;
	u8_t   acpi_disable;
	u8_t   pm1_cnt_len;

	/* ACPI details to be able to shutdown the system */
	facp = acpi_find_resource("FACP");
	if (!facp) return;
	dsdt_pa = facp->dsdt;
	assert(dsdt_pa);
	dsdt = (struct acpi_header *)device_map_mem(dsdt_pa, 0);
	if (acpi_chk_header(dsdt, "DSDT")) {
		dsdt = NULL;
		return;
	}

	printk("\tFound ACPI FACP and DSDT\n");

	dsdt_len = dsdt->len    - DSDT_DEFBLK_OFFSET; /* this is the length of the s5 AML script */
	s5_addr  = (char *)dsdt + DSDT_DEFBLK_OFFSET;

	/* go till we have 4 bytes left as the signature, "_S5_", takes 4 bytes */
	while (dsdt_len >= 4) {
		/* find the correct script */
		if (!strncmp(s5_addr, "_S5_", 4)) break;
		s5_addr++;
		dsdt_len--;
	}
	if (dsdt_len < 4) {
		printk("\tCould not find the ACPI \"_S5_\" script for shutdown.\n");

		return;
	}

	/* check if \_S5 was found and that there is a valid AML structure */
	if (!((*(s5_addr - 1) == 0x08 || (*(s5_addr - 2) == 0x08 && *(s5_addr - 1) == '\\')) && *(s5_addr + 4) == 0x12)) {
		printk("\tACPI \"_S5_\" script invalid.\n");
		return;
	}

	/* Great, lets find the values to feed into ACPI to reboot */
	s5_addr += 5;
	s5_addr += ((*s5_addr &0xC0) >> 6) + 2; /* calculate PkgLength size */

	if (*s5_addr == 0x0A) s5_addr++; /* skip byteprefix */
	slp_type_a = *(s5_addr) << 10;
	s5_addr++;

	if (*s5_addr == 0x0A) s5_addr++; /* skip byteprefix */
	slp_type_b = *(s5_addr) << 10;

	smi_cmd = facp->smi_cmd;

	acpi_enable  = facp->acpi_enable;
	acpi_disable = facp->acpi_disable;

	pm1a_cnt = facp->pm1a_cnt_blk;
	pm1b_cnt = facp->pm1b_cnt_blk;

	pm1_cnt_len = facp->pm1_cnt_len;

	slp_en = 1<<13;

	shutdown_ready = 1;

	printk("\tFACP and DSDT parsed and will be used for shutdown\n");

	return;
}

void
acpi_shutdown(void)
{
	if (!shutdown_ready) return;

	/* Enable ACPI. First, check if acpi is already enabled */
	if (!inw((unsigned int) pm1a_cnt)) {
		u32_t i;
		u32_t spin_until = ~0;

		/* check if acpi can be enabled */
		if (smi_cmd == 0 || acpi_enable == 0) return;

		outb((unsigned int)smi_cmd, acpi_enable); /* send acpi enable command */
		/* takes up to 3 seconds time to enable acpi? */
		for (i = 0; i < spin_until; i++) {
			if (inw((unsigned int)pm1a_cnt) == 1) break;
			//sleep(10);
		}
		if (pm1b_cnt != 0) {
			for (; i < spin_until; i++ ) {
				if (inw((unsigned int)pm1b_cnt) == 1) break;
				//sleep(10);
			}
		}
		if (i == spin_until) return; /* couldn't enable in time */
	}


	/* Found everything we need from the dsdt, now shutdown! */
	outw((unsigned int)pm1a_cnt, slp_type_a | slp_en );
	if (pm1b_cnt != 0) {
		outw((unsigned int)pm1b_cnt, slp_type_b | slp_en);
	}
}

extern unsigned long kernel_mapped_offset;

void
acpi_init(void)
{
	u32_t page;
	void *timer;
	void *apic;
	int lapic_err = 1;
	void *hpet = NULL;
	unsigned long j = kernel_mapped_offset;

	boot_state_assert(INIT_UT_MEM);
	assert(j < PAGE_SIZE / sizeof(unsigned long));

	/* FIXME: Ugly hack to get the physical page with the ACPI RSDT mapped */
	printk("ACPI initialization\n");
	rsdt = NULL;
	rsdt = acpi_find_rsdt(); /* update global */
	if (!rsdt) {
		printk("\tCould not find the ACPI RSDT; not using ACPI.");
		return; 	/* No acpi? */
	}

	printk("\tRSDT vaddr is @ %p\n", rsdt);
	timer = acpi_find_timer();
	if (timer) {
		hpet = timer_initialize_hpet(timer);
	}
	if (!hpet) {
		printk("Could not initialize HPET.\n");
	}

	apic = acpi_find_apic();
	if (apic) {
		lapic_err = lapic_find_localaddr(apic);
	}
	assert(!lapic_err);

	acpi_shutdown_init();

	return;
}