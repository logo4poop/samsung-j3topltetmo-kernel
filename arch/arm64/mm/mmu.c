/*
 * Based on arch/arm/mm/mmu.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/memblock.h>
#include <linux/fs.h>
#include <linux/io.h>

#include <asm/cputype.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/sizes.h>
#include <asm/tlb.h>
#include <asm/memblock.h>
#include <asm/mmu_context.h>
#include <asm/map.h>

#include "mm.h"

#include <linux/vmalloc.h>

#if defined(CONFIG_ECT)
#include <soc/samsung/ect_parser.h>
#endif

static int iotable_on;
#ifdef CONFIG_KNOX_KAP
extern int boot_mode_security;
#endif
/*
 * Empty_zero_page is a special page that is used for zero-initialized data
 * and COW.
 */
struct page *empty_zero_page;
EXPORT_SYMBOL(empty_zero_page);

struct cachepolicy {
	const char	policy[16];
	u64		mair;
	u64		tcr;
};

static struct cachepolicy cache_policies[] __initdata = {
	{
		.policy		= "uncached",
		.mair		= 0x44,			/* inner, outer non-cacheable */
		.tcr		= TCR_IRGN_NC | TCR_ORGN_NC,
	}, {
		.policy		= "writethrough",
		.mair		= 0xaa,			/* inner, outer write-through, read-allocate */
		.tcr		= TCR_IRGN_WT | TCR_ORGN_WT,
	}, {
		.policy		= "writeback",
		.mair		= 0xee,			/* inner, outer write-back, read-allocate */
		.tcr		= TCR_IRGN_WBnWA | TCR_ORGN_WBnWA,
	}
};

/*
 * These are useful for identifying cache coherency problems by allowing the
 * cache or the cache and writebuffer to be turned off. It changes the Normal
 * memory caching attributes in the MAIR_EL1 register.
 */
static int __init early_cachepolicy(char *p)
{
	int i;
	u64 tmp;

	for (i = 0; i < ARRAY_SIZE(cache_policies); i++) {
		int len = strlen(cache_policies[i].policy);

		if (memcmp(p, cache_policies[i].policy, len) == 0)
			break;
	}
	if (i == ARRAY_SIZE(cache_policies)) {
		pr_err("ERROR: unknown or unsupported cache policy: %s\n", p);
		return 0;
	}

	flush_cache_all();

	/*
	 * Modify MT_NORMAL attributes in MAIR_EL1.
	 */
	asm volatile(
	"	mrs	%0, mair_el1\n"
	"	bfi	%0, %1, %2, #8\n"
	"	msr	mair_el1, %0\n"
	"	isb\n"
	: "=&r" (tmp)
	: "r" (cache_policies[i].mair), "i" (MT_NORMAL * 8));

	/*
	 * Modify TCR PTW cacheability attributes.
	 */
	asm volatile(
	"	mrs	%0, tcr_el1\n"
	"	bic	%0, %0, %2\n"
	"	orr	%0, %0, %1\n"
	"	msr	tcr_el1, %0\n"
	"	isb\n"
	: "=&r" (tmp)
	: "r" (cache_policies[i].tcr), "r" (TCR_IRGN_MASK | TCR_ORGN_MASK));

	flush_cache_all();

	return 0;
}
early_param("cachepolicy", early_cachepolicy);

pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
			      unsigned long size, pgprot_t vma_prot)
{
	if (!pfn_valid(pfn))
		return pgprot_noncached(vma_prot);
	else if (file->f_flags & O_SYNC)
		return pgprot_writecombine(vma_prot);
	return vma_prot;
}
EXPORT_SYMBOL(phys_mem_access_prot);

static void __init *early_alloc(unsigned long sz)
{
	void *ptr = __va(memblock_alloc(sz, sz));
	memset(ptr, 0, sz);
	return ptr;
}

#ifdef CONFIG_TIMA_RKP
/* Extra memory needed by VMM */
void* vmm_extra_mem = 0;
spinlock_t ro_rkp_pages_lock = __SPIN_LOCK_UNLOCKED();
char ro_pages_stat[RO_PAGES] = {0};
unsigned ro_alloc_last = 0; 
int rkp_ro_mapped = 0; 
int ro_buf_done = 0;

void *rkp_ro_alloc(void)
{
	unsigned long flags;
	int i, j;
	void * alloc_addr = NULL;
	if (!ro_buf_done)
		return alloc_addr;
	
	spin_lock_irqsave(&ro_rkp_pages_lock,flags);
	
	for (i = 0, j = ro_alloc_last; i < (RO_PAGES) ; i++) {
		j =  (j+1) %(RO_PAGES); 
		if (!ro_pages_stat[j]) {
			ro_pages_stat[j] = 1;
			ro_alloc_last = j+1;
			alloc_addr = (void*) ((u64)RKP_RBUF_VA +  (j << PAGE_SHIFT));
			break;
		}
	}
	spin_unlock_irqrestore(&ro_rkp_pages_lock,flags);
	
	return alloc_addr;
}

void rkp_ro_free(void *free_addr)
{
	int i;
	unsigned long flags;

	i =  ((u64)free_addr - (u64)RKP_RBUF_VA) >> PAGE_SHIFT;
	spin_lock_irqsave(&ro_rkp_pages_lock,flags);
	ro_pages_stat[i] = 0;
	ro_alloc_last = i; 
	spin_unlock_irqrestore(&ro_rkp_pages_lock,flags);
}

unsigned int is_rkp_ro_page(u64 addr)
{
	if( (addr >= (u64)RKP_RBUF_VA)
		&& (addr < (u64)(RKP_RBUF_VA+ TIMA_ROBUF_SIZE)))
		return 1;
	else return 0;
}
/* we suppose the whole block remap to page table should start with block borders */
static inline void __init block_to_pages(pmd_t *pmd, unsigned long addr,
				  unsigned long end, unsigned long pfn)
{
	pte_t *old_pte;  
	pmd_t new ; 
	pte_t *pte = rkp_ro_alloc();	

	if (!pte)
		pte = (pte_t *)early_alloc(PAGE_SIZE);

	old_pte = pte;

	__pmd_populate(&new, __pa(pte), PMD_TYPE_TABLE); /* populate to temporary pmd */

	pte = pte_offset_kernel(&new, addr);

	do {		
		if (iotable_on == 1)
			set_pte(pte, pfn_pte(pfn, PROT_NORMAL_NC));
		else
			set_pte(pte, pfn_pte(pfn, PAGE_KERNEL_EXEC));
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);

	__pmd_populate(pmd, __pa(old_pte), PMD_TYPE_TABLE);
}
#endif
static void __init alloc_init_pte(pmd_t *pmd, unsigned long addr,
				  unsigned long end, unsigned long pfn,
				  pgprot_t prot)
{
	pte_t *pte;

#ifdef CONFIG_TIMA_RKP
#ifdef CONFIG_KNOX_KAP
	if(boot_mode_security)
#endif
	if(pmd_block(*pmd))
		return block_to_pages(pmd, addr, end, pfn);
#endif
	if (pmd_none(*pmd)) {
		pte = early_alloc(PTRS_PER_PTE * sizeof(pte_t));
		__pmd_populate(pmd, __pa(pte), PMD_TYPE_TABLE);
	}
#ifdef CONFIG_TIMA_RKP
#ifdef CONFIG_KNOX_KAP
	if (!boot_mode_security)
		BUG_ON(pmd_bad(*pmd));
#endif
#else
	BUG_ON(pmd_bad(*pmd));
#endif

	pte = pte_offset_kernel(pmd, addr);
	do {
		if (iotable_on == 1)
			set_pte(pte, pfn_pte(pfn, pgprot_iotable_init(PAGE_KERNEL_EXEC)));
		else
			set_pte(pte, pfn_pte(pfn, prot));
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
}

static void __init alloc_init_pmd(pud_t *pud, unsigned long addr,
				  unsigned long end, phys_addr_t phys,
				  int map_io)
{
	pmd_t *pmd;
	unsigned long next;
#ifdef CONFIG_TIMA_RKP
	int rkp_do = 0;
#endif
	pmdval_t prot_sect;
	pgprot_t prot_pte;

	if (map_io) {
		prot_sect = PROT_SECT_DEVICE_nGnRE;
		prot_pte = __pgprot(PROT_DEVICE_nGnRE);
	} else {
		prot_sect = PROT_SECT_NORMAL_EXEC;
		prot_pte = PAGE_KERNEL_EXEC;
	}

	/*
	 * Check for initial section mappings in the pgd/pud and remove them.
	 */
	if (pud_none(*pud) || pud_bad(*pud)) {
#ifdef CONFIG_TIMA_RKP
#ifdef CONFIG_KNOX_KAP
		if (boot_mode_security)
#endif
			rkp_do = 1;

		if( rkp_do && ro_buf_done ){
			pmd = rkp_ro_alloc();
		}else{
			pmd = early_alloc(PTRS_PER_PMD * sizeof(pmd_t));
		}
#else	/* !CONFIG_TIMA_RKP */
		pmd = early_alloc(PTRS_PER_PMD * sizeof(pmd_t));
#endif
		pud_populate(&init_mm, pud, pmd);
	}

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		/* try section mapping first */
		if (((addr | next | phys) & ~SECTION_MASK) == 0) {
			pmd_t old_pmd =*pmd;
			set_pmd(pmd, __pmd(phys | prot_sect));
			if (iotable_on == 1)
				set_pmd(pmd, __pmd(phys | PROT_SECT_NORMAL_NC));
			else
				set_pmd(pmd, __pmd(phys | prot_sect));
			/*
			 * Check for previous table entries created during
			 * boot (__create_page_tables) and flush them.
			 */
			if (!pmd_none(old_pmd))
				flush_tlb_all();
		} else {
			alloc_init_pte(pmd, addr, next, __phys_to_pfn(phys),
				       prot_pte);
		}
		phys += next - addr;
	} while (pmd++, addr = next, addr != end);
}

static void __init alloc_init_pud(pgd_t *pgd, unsigned long addr,
				  unsigned long end, phys_addr_t phys,
				  int map_io)
{
	pud_t *pud;
	unsigned long next;

	if (pgd_none(*pgd)) {
		pud = early_alloc(PTRS_PER_PUD * sizeof(pud_t));
		pgd_populate(&init_mm, pgd, pud);
	}
	BUG_ON(pgd_bad(*pgd));

	pud = pud_offset(pgd, addr);
	do {
		next = pud_addr_end(addr, end);

		/*
		 * For 4K granule only, attempt to put down a 1GB block
		 */
		if (!map_io && (PAGE_SHIFT == 12) &&
		    ((addr | next | phys) & ~PUD_MASK) == 0) {
			pud_t old_pud = *pud;
			set_pud(pud, __pud(phys | PROT_SECT_NORMAL_EXEC));

			/*
			 * If we have an old value for a pud, it will
			 * be pointing to a pmd table that we no longer
			 * need (from swapper_pg_dir).
			 *
			 * Look up the old pmd table and free it.
			 */
			if (!pud_none(old_pud)) {
				phys_addr_t table = __pa(pmd_offset(&old_pud, 0));
				memblock_free(table, PAGE_SIZE);
				flush_tlb_all();
			}
		} else {
			alloc_init_pmd(pud, addr, next, phys, map_io);
		}
		phys += next - addr;
	} while (pud++, addr = next, addr != end);
}

/*
 * Create the page directory entries and any necessary page tables for the
 * mapping specified by 'md'.
 */
static void __init __create_mapping(pgd_t *pgd, phys_addr_t phys,
				    unsigned long virt, phys_addr_t size,
				    int map_io)
{
	unsigned long addr, length, end, next;

	addr = virt & PAGE_MASK;
	length = PAGE_ALIGN(size + (virt & ~PAGE_MASK));

	end = addr + length;
	do {
		next = pgd_addr_end(addr, end);
		alloc_init_pud(pgd, addr, next, phys, map_io);
		phys += next - addr;
	} while (pgd++, addr = next, addr != end);
}

static void __init create_mapping(phys_addr_t phys, unsigned long virt,
				  phys_addr_t size)
{
	if (virt < VMALLOC_START) {
		pr_warn("BUG: not creating mapping for %pa at 0x%016lx - outside kernel range\n",
			&phys, virt);
		return;
	}
	__create_mapping(pgd_offset_k(virt & PAGE_MASK), phys, virt, size, 0);
}

void __init create_id_mapping(phys_addr_t addr, phys_addr_t size, int map_io)
{
	if ((addr >> PGDIR_SHIFT) >= ARRAY_SIZE(idmap_pg_dir)) {
		pr_warn("BUG: not creating id mapping for %pa\n", &addr);
		return;
	}
	__create_mapping(&idmap_pg_dir[pgd_index(addr)],
			 addr, addr, size, map_io);
}

static void __init map_mem(void)
{
	struct memblock_region *reg;
	phys_addr_t limit;
	phys_addr_t start;
	phys_addr_t end;

#ifdef CONFIG_TIMA_RKP
	phys_addr_t mid = 0x60000000;
	int rkp_do = 0;
#endif
	/*
	 * Temporarily limit the memblock range. We need to do this as
	 * create_mapping requires puds, pmds and ptes to be allocated from
	 * memory addressable from the initial direct kernel mapping.
	 *
	 * The initial direct kernel mapping, located at swapper_pg_dir, gives
	 * us PUD_SIZE (4K pages) or PMD_SIZE (64K pages) memory starting from
	 * PHYS_OFFSET (which must be aligned to 2MB as per
	 * Documentation/arm64/booting.txt).
	 */
	if (IS_ENABLED(CONFIG_ARM64_64K_PAGES))
		limit = PHYS_OFFSET + PMD_SIZE;
	else
		limit = PHYS_OFFSET + PUD_SIZE;
	memblock_set_current_limit(limit);

	/* map all the memory banks */
	for_each_memblock(memory, reg) {
		start = reg->base;
		end = start + reg->size;

		if (start >= end)
			break;

#ifndef CONFIG_ARM64_64K_PAGES
		/*
		 * For the first memory bank align the start address and
		 * current memblock limit to prevent create_mapping() from
		 * allocating pte page tables from unmapped memory.
		 * When 64K pages are enabled, the pte page table for the
		 * first PGDIR_SIZE is already present in swapper_pg_dir.
		 */
		if (start < limit)
			start = ALIGN(start, PMD_SIZE);
		if (end < limit) {
			limit = end & PMD_MASK;
			memblock_set_current_limit(limit);
		}
#endif

#ifdef CONFIG_TIMA_RKP
#ifdef CONFIG_KNOX_KAP
	if (boot_mode_security)
#endif
		rkp_do = 1;

	if( rkp_do ){
		if (((u64)start < TIMA_ROBUF_START) && ((u64)end > TIMA_ROBUF_START)) {
			create_mapping(start, __phys_to_virt(start), mid - start);
			memset((void*)RKP_RBUF_VA, 0, TIMA_ROBUF_SIZE);
			ro_buf_done = 1;
			create_mapping(mid, __phys_to_virt(mid), end - mid);
		}
		else {
			create_mapping(start, __phys_to_virt(start), end - start);
		}
	}else{
		create_mapping(start, __phys_to_virt(start), end - start);
	}
#else /* !CONFIG_TIMA_RKP */
		create_mapping(start, __phys_to_virt(start), end - start);
#endif
	}

#ifdef CONFIG_TIMA_RKP
#ifdef CONFIG_KNOX_KAP
	if (boot_mode_security) {
#endif
		vmm_extra_mem = early_alloc(0x600000);
#ifdef CONFIG_KNOX_KAP
	}
#endif
#endif
	/* Limit no longer required. */
	memblock_set_current_limit(MEMBLOCK_ALLOC_ANYWHERE);
}

/*
 * paging_init() sets up the page tables, initialises the zone memory
 * maps and sets up the zero page.
 */
void __init paging_init(void)
{
	void *zero_page;
#ifdef CONFIG_TIMA_RKP
	int rkp_do =  0;
#endif
	map_mem();

#if defined(CONFIG_ECT)
	ect_init_map_io();
#endif

	/*
	 * Finally flush the caches and tlb to ensure that we're in a
	 * consistent state.
	 */
	flush_cache_all();
	flush_tlb_all();

	/* allocate the zero page. */
#ifdef CONFIG_TIMA_RKP
#ifdef CONFIG_KNOX_KAP
	if (boot_mode_security)
#endif
		rkp_do = 1;
	if (rkp_do && ro_buf_done)
		zero_page = rkp_ro_alloc();
	else
		zero_page = early_alloc(PAGE_SIZE);
#else	/* !CONFIG_TIMA_RKP */
	zero_page = early_alloc(PAGE_SIZE);
#endif

	bootmem_init();

	empty_zero_page = virt_to_page(zero_page);

	/*
	 * TTBR0 is only used for the identity mapping at this stage. Make it
	 * point to zero page to avoid speculatively fetching new entries.
	 */
	cpu_set_reserved_ttbr0();
	flush_tlb_all();
}

/*
 * Enable the identity mapping to allow the MMU disabling.
 */
void setup_mm_for_reboot(void)
{
	cpu_switch_mm(idmap_pg_dir, &init_mm);
	flush_tlb_all();
}

/*
 * Check whether a kernel address is valid (derived from arch/x86/).
 */
int kern_addr_valid(unsigned long addr)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if ((((long)addr) >> VA_BITS) != -1UL)
		return 0;

	pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd))
		return 0;

	pud = pud_offset(pgd, addr);
	if (pud_none(*pud))
		return 0;

	if (pud_sect(*pud))
		return pfn_valid(pud_pfn(*pud));

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return 0;

	if (pmd_sect(*pmd))
		return pfn_valid(pmd_pfn(*pmd));

	pte = pte_offset_kernel(pmd, addr);
	if (pte_none(*pte))
		return 0;

	return pfn_valid(pte_pfn(*pte));
}
#ifdef CONFIG_SPARSEMEM_VMEMMAP
#ifdef CONFIG_ARM64_64K_PAGES
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node)
{
	return vmemmap_populate_basepages(start, end, node);
}
#else	/* !CONFIG_ARM64_64K_PAGES */
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node)
{
	unsigned long addr = start;
	unsigned long next;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	do {
		next = pmd_addr_end(addr, end);

		pgd = vmemmap_pgd_populate(addr, node);
		if (!pgd)
			return -ENOMEM;

		pud = vmemmap_pud_populate(pgd, addr, node);
		if (!pud)
			return -ENOMEM;

		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd)) {
			void *p = NULL;

			p = vmemmap_alloc_block_buf(PMD_SIZE, node);
			if (!p)
				return -ENOMEM;

			set_pmd(pmd, __pmd(__pa(p) | PROT_SECT_NORMAL));
		} else
			vmemmap_verify((pte_t *)pmd, node, addr, next);
	} while (addr = next, addr != end);

	return 0;
}
#endif	/* CONFIG_ARM64_64K_PAGES */
void vmemmap_free(unsigned long start, unsigned long end)
{
}
#endif	/* CONFIG_SPARSEMEM_VMEMMAP */

/* For compatible with Exynos */

LIST_HEAD(static_vmlist);

void __init add_static_vm_early(struct static_vm *svm)
{
	struct static_vm *curr_svm;
	struct vm_struct *vm;
	void *vaddr;

	vm = &svm->vm;
	vm_area_add_early(vm);
	vaddr = vm->addr;

	list_for_each_entry(curr_svm, &static_vmlist, list) {
		vm = &curr_svm->vm;

		if (vm->addr > vaddr)
			break;
	}
	list_add_tail(&svm->list, &curr_svm->list);
}

static void __init *early_alloc_aligned(unsigned long sz, unsigned long align)
{
	void *ptr = __va(memblock_alloc(sz, align));
	memset(ptr, 0, sz);
	return ptr;
}

/*
 * Create the architecture specific mappings
 */
static void __init __iotable_init(struct map_desc *io_desc, int nr, int exec)
{
	struct map_desc *md;
	struct vm_struct *vm;
	struct static_vm *svm;
	phys_addr_t phys;
	pgprot_t prot;
	void *vm_caller;

	if (!nr)
		return;

	svm = early_alloc_aligned(sizeof(*svm) * nr, __alignof__(*svm));

	if (!exec) {
		iotable_on = 1;
		prot = pgprot_iotable_init(PAGE_KERNEL_EXEC);
		vm_caller = iotable_init;
	} else {
		iotable_on = 0;
		prot = PAGE_KERNEL_EXEC;
		vm_caller = iotable_init_exec;
	}

	for (md = io_desc; nr; md++, nr--) {
		phys = __pfn_to_phys(md->pfn);
		create_mapping(phys, md->virtual, md->length);
		vm = &svm->vm;
		vm->addr = (void *)(md->virtual & PAGE_MASK);
		vm->size = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));
		vm->phys_addr = __pfn_to_phys(md->pfn);
		vm->flags = VM_IOREMAP | VM_ARM_STATIC_MAPPING;
		vm->flags |= VM_ARM_MTYPE(md->type);
		vm->caller = iotable_init;
		add_static_vm_early(svm++);
	}

	iotable_on = 0;
}

void __init iotable_init(struct map_desc *io_desc, int nr)
{
	__iotable_init(io_desc, nr, 0);
}

void __init iotable_init_exec(struct map_desc *io_desc, int nr)
{
	__iotable_init(io_desc, nr, 1);
}
