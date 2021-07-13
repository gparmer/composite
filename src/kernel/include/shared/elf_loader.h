#ifndef SIMPLE_ELF_H
#define SIMPLE_ELF_H

/*
 * Simple ELF object parser that focuses on only reading the program
 * headers required to get the object's memory, and the information to
 * load it into a new address space.  This includes information for a
 * number of contiguous loadable regions with: virtual address, offset
 * in the elf object, size in the elf object, size in memory (which,
 * due to BSS might be larger than it is in the object), and access
 * rights (read, write, execute).
 */

#include <cos_types.h>

/* ELF File Header */
struct elf_hdr {
	unsigned char e_ident[16];       /* Magic number and other info */
	u16_t         e_type;            /* Object file type */
	u16_t         e_machine;         /* Architecture */
	u32_t         e_version;         /* Object file version */
	u32_t         e_entry;           /* Entry point virtual address */
	u32_t         e_phoff;           /* Program header table file offset */
	u32_t         e_shoff;           /* Section header table file offset */
	u32_t         e_flags;           /* Processor-specific flags */
	u16_t         e_ehsize;          /* ELF header size in bytes */
	u16_t         e_phentsize;       /* Program header table entry size */
	u16_t         e_phnum;           /* Program header table entry count */
	u16_t         e_shentsize;       /* Section header table entry size */
	u16_t         e_shnum;           /* Section header table entry count */
	u16_t         e_shstrndx;        /* Section header string table index */
};

#define ELF_PH_LOAD 0x1

struct elf32_proghdr {
	u32_t    p_type;                 /* Segment type */
	u32_t    p_offset;               /* Segment file offset */
	u32_t    p_vaddr;                /* Segment virtual address */
	u32_t    p_paddr;                /* Segment physical address */
	u32_t    p_filesz;               /* Segment size in file */
	u32_t    p_memsz;                /* Segment size in memory */
	u32_t    p_flags;                /* Segment flags */
	u32_t    p_align;                /* Segment alignment */
};

typedef enum {
	ELF_PH_X = 0x1,
	ELF_PH_W = 0x2,
	ELF_PH_R = 0x4,
	ELF_PH_RW = (ELF_PH_R | ELF_PH_W),
	ELF_PH_CODE = (ELF_PH_R | ELF_PH_X)
} elf_memaccess_t;

struct elf_contig_mem {
	vaddr_t vstart; 	         /* starting virtual address */
	void   *mem;		         /* address of the memory */
	unsigned long objsz, sz;         /* size of the memory in obj, and in mem */
	elf_memaccess_t access;
};

static inline int
elf_chk_format(struct elf_hdr *hdr)
{
	unsigned char *s = hdr->e_ident;

	/* Format:  "0x7fELF1" where the 1 is for 32 bit */
	if (s[0] == 0x7f && s[1] == 'E' && s[2] == 'L' && s[3] == 'F' && s[4] == 1) return 1;
	if (s[0] == 0x7f && s[1] == 'E' && s[2] == 'L' && s[3] == 'F' && s[4] == 2) return 2;
	return -1;
}


/*
 * Get the nmem-th contiguous memory region in the binary.  Return -1
 * if the elf object isn't shaped correctly, 1 if there aren't `nmem`
 * regions, and 0 if `mem` is properly populated.
 */
static inline int
elf32_contig_mem(struct elf_hdr *hdr, unsigned int nmem, struct elf_contig_mem *mem)
{
	struct elf32_proghdr *proghdr, *memsect = NULL;
	u32_t off;
	unsigned int i, cntmem;

	if (elf_chk_format(hdr) != 1 ||
	    hdr->e_phentsize != sizeof(struct elf32_proghdr)) return -1;

	proghdr = (struct elf32_proghdr *)((char *)hdr + hdr->e_phoff);

	for (i = 0, cntmem = 0; i < hdr->e_phnum && cntmem <= nmem; i++) {
		if (proghdr[i].p_type != ELF_PH_LOAD) continue;
		if (cntmem == nmem) {
			memsect = &proghdr[i];
			break;
		}
		cntmem++;
	}
	if (memsect == NULL) return 1;

	*mem = (struct elf_contig_mem){
		.vstart = memsect->p_vaddr,
		.mem    = (char *)hdr + memsect->p_offset,
		.objsz  = memsect->p_filesz,
		.sz     = memsect->p_memsz,
		.access = memsect->p_flags
	};

	return 0;
}

/*
 * This is the elf_64 related code
 */
struct elf64_hdr{
        unsigned char   e_ident[16]; 
        u16_t           e_type;
        u16_t           e_machine;
        u32_t           e_version;
        u64_t           e_entry;
        u64_t           e_phoff;
        u64_t           e_shoff;
        u32_t           e_flags;
        u16_t           e_ehsize;
        u16_t           e_phentsize;
        u16_t           e_phnum;
        u16_t           e_shentsize;
        u16_t           e_shnum;
        u16_t           e_shstrndx;
};


struct elf64_proghdr {
        u32_t    p_type;
        u32_t    p_flags;
        u64_t    p_offset;
        u64_t    p_vaddr;
        u64_t    p_paddr;
        u64_t    p_filesz;
        u64_t    p_memsz;
        u64_t    p_align;
};


static inline int
elf64_contig_mem(struct elf64_hdr *hdr, unsigned int nmem, struct elf_contig_mem *mem)
{
	struct elf64_proghdr *proghdr, *memsect = NULL;
	u32_t off;
	unsigned int i, cntmem;

	if (elf_chk_format((struct elf_hdr *)hdr) != 2||
	    hdr->e_phentsize != sizeof(struct elf64_proghdr)) return -1;

	proghdr = (struct elf64_proghdr *)((char *)hdr + hdr->e_phoff);

	for (i = 0, cntmem = 0; i < hdr->e_phnum && cntmem <= nmem; i++) {
		if (proghdr[i].p_type != ELF_PH_LOAD) continue;
		if (cntmem == nmem) {
			memsect = &proghdr[i];
			break;
		}
		cntmem++;
	}
	if (memsect == NULL) return 1;

	*mem = (struct elf_contig_mem){
		.vstart = memsect->p_vaddr,
		.mem    = (char *)hdr + memsect->p_offset,
		.objsz  = memsect->p_filesz,
		.sz     = memsect->p_memsz,
		.access = memsect->p_flags
	};
	return 0;
}

/*
 * Returns the address of the entry function, or 0 on error.
 */
static inline vaddr_t
elf_entry_addr(struct elf_hdr *hdr)
{
	if (elf_chk_format(hdr) == 1){
		return (vaddr_t)hdr->e_entry;
	} else if(elf_chk_format(hdr) == 2){
		return (vaddr_t)(((struct elf64_hdr* )hdr)->e_entry);
	}

	return 0;
}

/*
 * Get the nmem-th contiguous memory region in the binary.  Return -1
 * if the elf object isn't shaped correctly, 1 if there aren't `nmem`
 * regions, and 0 if `mem` is properly populated.
 */
static inline int
elf_contig_mem(struct elf_hdr *hdr, unsigned int nmem, struct elf_contig_mem *mem)
{
	int ret = elf_chk_format(hdr);
	if (ret == 1){
		return elf32_contig_mem(hdr, nmem, mem);
	} else if (ret == 2) {
		return elf64_contig_mem((struct elf64_hdr *)hdr, nmem, mem);
	}
	return -1;
}

/*
 * This function ASSUMES that the elf object has only two sections,
 * one for RO data, and one for RW. It assumes the RO section appears
 * first in the object. It will alias the RO section from the elf
 * object, but will allocate new memory for the data + BSS sections.
 *
 * return -1 if this assumption doesn't hold, or the elf is corrupted.
 * return 0 on success, populating the return values. With these
 * values, creation of the component is simple:
 *
 * mmap(..., ro_src, ro_sz);
 * mem = alloc(data_sz + bss_sz);
 * memcpy(mem, data_src, data_sz);
 * memset(mem + data_sz, 0, bss_sz);
 * mmap(..., mem, data_sz + bss_sz);
 *
 * Note that separate allocation of RO/RW pages is a straightforward extension.
 */
static inline int
elf_load_info(struct elf_hdr *hdr, vaddr_t *ro_addr, size_t *ro_sz, char **ro_src,
	      vaddr_t *rw_addr, size_t *data_sz, char **data_src, size_t *bss_sz)
{
	struct elf_contig_mem s[2] = {};

	/* RO + Code */
	if (elf_contig_mem(hdr, 0, &s[0]) ||
	    s[0].objsz != s[0].sz || s[0].access != ELF_PH_CODE) return -1;
	/* Data + BSS, note that the data should immediately follow the code */
	if (elf_contig_mem(hdr, 1, &s[1]) ||
	    s[1].access != ELF_PH_RW || round_up_to_page(s[0].vstart + s[0].sz) != s[1].vstart) return -1;

	*ro_addr  = s[0].vstart;
	*ro_sz    = s[0].sz;
	*ro_src   = s[0].mem;

	*rw_addr  = s[1].vstart;
	*data_sz  = s[1].objsz;
	*data_src = s[1].mem;
	*bss_sz   = s[1].sz - *data_sz;

	return 0;
}

#endif	/* SIMPLE_ELF_H */
