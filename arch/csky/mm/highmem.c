/*
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2011 C-SKY Microsystems co.,ltd.  All rights reserved.
 * Copyright (C) 2011 Hu Junshan(junshan_hu@c-sky.com)
 */

#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/smp.h>
#include <linux/bootmem.h>
#include <asm/fixmap.h>
#include <asm/tlbflush.h>
#include <asm/cacheflush.h>

static pte_t *kmap_pte;

unsigned long highstart_pfn, highend_pfn;

void *kmap(struct page *page)
{
	void *addr;

	might_sleep();
	if (!PageHighMem(page))
		return page_address(page);
	addr = kmap_high(page);
	flush_tlb_one((unsigned long)addr);

	return addr;
}
EXPORT_SYMBOL(kmap);

void kunmap(struct page *page)
{
	BUG_ON(in_interrupt());
	if (!PageHighMem(page))
		return;
	kunmap_high(page);
#ifdef __CSKYABIV1__
	flush_dcache_page(page);
#endif
}
EXPORT_SYMBOL(kunmap);

/*
 * kmap_atomic/kunmap_atomic is significantly faster than kmap/kunmap because
 * no global lock is needed and because the kmap code must perform a global TLB
 * invalidation when the kmap pool wraps.
 *
 * However when holding an atomic kmap is is not legal to sleep, so atomic
 * kmaps are appropriate for short, tight code paths only.
 */

void *kmap_atomic(struct page *page)
{
	unsigned long vaddr;
	int idx, type;

	/* even !CONFIG_PREEMPT needs this, for in_atomic in do_page_fault */
	pagefault_disable();
	if (!PageHighMem(page))
		return page_address(page);

	type = kmap_atomic_idx_push();
	idx = type + KM_TYPE_NR*smp_processor_id();
	vaddr = __fix_to_virt(FIX_KMAP_BEGIN + idx);
#ifdef CONFIG_DEBUG_HIGHMEM
	BUG_ON(!pte_none(*(kmap_pte - idx)));
#endif
	set_pte(kmap_pte-idx, mk_pte(page, PAGE_KERNEL));
	local_flush_tlb_one((unsigned long)vaddr);
#ifdef __CSKYABIV1__
	flush_dcache_page(page);
#endif

	return (void*) vaddr;
}
EXPORT_SYMBOL(kmap_atomic);

void __kunmap_atomic(void *kvaddr)
{
	unsigned long vaddr = (unsigned long) kvaddr & PAGE_MASK;
	int type;
	
	if (vaddr < FIXADDR_START) { // FIXME
	    pagefault_enable();
	    return;
	}
	
	type = kmap_atomic_idx();
#ifdef CONFIG_DEBUG_HIGHMEM
	int idx = type + KM_TYPE_NR * smp_processor_id();

	BUG_ON(vaddr != __fix_to_virt(FIX_KMAP_BEGIN + idx));

	/*
	 * force other mappings to Oops if they'll try to access
	 * this pte without first remap it
	 */
	pte_clear(&init_mm, vaddr, kmap_pte-idx);
	local_flush_tlb_one(vaddr);
#endif

	kmap_atomic_idx_pop();
	pagefault_enable();
}
EXPORT_SYMBOL(__kunmap_atomic);

/*
 * This is the same as kmap_atomic() but can map memory that doesn't
 * have a struct page associated with it.
 */
void *kmap_atomic_pfn(unsigned long pfn)
{
	unsigned long vaddr;
	int idx, type;

	pagefault_disable();

	type = kmap_atomic_idx_push();
	idx = type + KM_TYPE_NR*smp_processor_id();
	vaddr = __fix_to_virt(FIX_KMAP_BEGIN + idx);
	set_pte(kmap_pte-idx, pfn_pte(pfn, PAGE_KERNEL));
	flush_tlb_one(vaddr);
#ifdef __CSKYABIV1__
	flush_cache_range(NULL, vaddr, ((unsigned long)vaddr + PAGE_SIZE));
#endif

	return (void*) vaddr;
}

struct page *kmap_atomic_to_page(void *ptr)
{
	unsigned long idx, vaddr = (unsigned long)ptr;
	pte_t *pte;

	if (vaddr < FIXADDR_START)
		return virt_to_page(ptr);

	idx = virt_to_fix(vaddr);
	pte = kmap_pte - (idx - FIX_KMAP_BEGIN);
	return pte_page(*pte);
}

static void __init fixrange_init (unsigned long start, unsigned long end,
                   pgd_t *pgd_base)
{
#ifdef CONFIG_HIGHMEM
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int i, j, k;
	unsigned long vaddr;

	vaddr = start;
	i = __pgd_offset(vaddr);
	j = __pud_offset(vaddr);
	k = __pmd_offset(vaddr);
	pgd = pgd_base + i;

	for ( ; (i < PTRS_PER_PGD) && (vaddr != end); pgd++, i++) {
		pud = (pud_t *)pgd;
		for ( ; (j < PTRS_PER_PUD) && (vaddr != end); pud++, j++) {
			pmd = (pmd_t *)pud;
			for (; (k < PTRS_PER_PMD) && (vaddr != end); pmd++, k++) {
				if (pmd_none(*pmd)) {
					pte = (pte_t *) alloc_bootmem_low_pages(PAGE_SIZE);
#ifdef CONFIG_MMU_HARD_REFILL
/* hard refill need fill PA. */
					set_pmd(pmd, __pmd(__pa(pte)));
#else
					set_pmd(pmd, __pmd((unsigned long)pte));
#endif
					BUG_ON(pte != pte_offset_kernel(pmd, 0));
				}
				vaddr += PMD_SIZE;
			}
			k = 0;
		}
		j = 0;
	}
#endif
}

void __init fixaddr_kmap_pages_init(void)
{
	unsigned long vaddr;
	pgd_t *pgd_base;
#ifdef CONFIG_HIGHMEM
	pgd_t *pgd;
	pmd_t *pmd;
	pud_t *pud;
	pte_t *pte;
#endif
	pgd_base = swapper_pg_dir;

	/*
	 * Fixed mappings:
	 */
	vaddr = __fix_to_virt(__end_of_fixed_addresses - 1) & PMD_MASK;
	fixrange_init(vaddr, 0, pgd_base);

#ifdef CONFIG_HIGHMEM
	/*
	 * Permanent kmaps:
	 */
	vaddr = PKMAP_BASE;
	fixrange_init(vaddr, vaddr + PAGE_SIZE*LAST_PKMAP, pgd_base);

	pgd = swapper_pg_dir + __pgd_offset(vaddr);
	pud = pud_offset(pgd, vaddr);
	pmd = pmd_offset(pud, vaddr);
	pte = pte_offset_kernel(pmd, vaddr);
	pkmap_page_table = pte;
#endif
}

void __init kmap_init(void)
{
	unsigned long kmap_vstart;

	fixaddr_kmap_pages_init();

	/* cache the first kmap pte */
	kmap_vstart = __fix_to_virt(FIX_KMAP_BEGIN);
	kmap_pte = kmap_get_fixmap_pte(kmap_vstart);
}
