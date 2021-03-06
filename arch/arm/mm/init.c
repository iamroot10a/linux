/*
 *  linux/arch/arm/mm/init.c
 *
 *  Copyright (C) 1995-2005 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/mman.h>
#include <linux/export.h>
#include <linux/nodemask.h>
#include <linux/initrd.h>
#include <linux/of_fdt.h>
#include <linux/highmem.h>
#include <linux/gfp.h>
#include <linux/memblock.h>
#include <linux/dma-contiguous.h>
#include <linux/sizes.h>

#include <asm/mach-types.h>
#include <asm/memblock.h>
#include <asm/prom.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/tlb.h>
#include <asm/fixmap.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include "mm.h"

static phys_addr_t phys_initrd_start __initdata = 0;
static unsigned long phys_initrd_size __initdata = 0;

static int __init early_initrd(char *p)
{
	phys_addr_t start;
	unsigned long size;
	char *endp;

	start = memparse(p, &endp);
	if (*endp == ',') {
		size = memparse(endp + 1, NULL);

		phys_initrd_start = start;
		phys_initrd_size = size;
	}
	return 0;
}
early_param("initrd", early_initrd);

static int __init parse_tag_initrd(const struct tag *tag)
{
	printk(KERN_WARNING "ATAG_INITRD is deprecated; "
		"please update your bootloader.\n");
	phys_initrd_start = __virt_to_phys(tag->u.initrd.start);
	phys_initrd_size = tag->u.initrd.size;
	return 0;
}

__tagtable(ATAG_INITRD, parse_tag_initrd);

static int __init parse_tag_initrd2(const struct tag *tag)
{
	phys_initrd_start = tag->u.initrd.start;
	phys_initrd_size = tag->u.initrd.size;
	return 0;
}

__tagtable(ATAG_INITRD2, parse_tag_initrd2);

#ifdef CONFIG_OF_FLATTREE
void __init early_init_dt_setup_initrd_arch(unsigned long start, unsigned long end)
{
	phys_initrd_start = start;
	phys_initrd_size = end - start;
}
#endif /* CONFIG_OF_FLATTREE */

/*
 * This keeps memory configuration data used by a couple memory
 * initialization functions, as well as show_mem() for the skipping
 * of holes in the memory map.  It is populated by arm_add_memory().
 */
struct meminfo meminfo;

void show_mem(unsigned int filter)
{
	int free = 0, total = 0, reserved = 0;
	int shared = 0, cached = 0, slab = 0, i;
	struct meminfo * mi = &meminfo;

	printk("Mem-info:\n");
	show_free_areas(filter);

	if (filter & SHOW_MEM_FILTER_PAGE_COUNT)
		return;

	for_each_bank (i, mi) {
		struct membank *bank = &mi->bank[i];
		unsigned int pfn1, pfn2;
		struct page *page, *end;

		pfn1 = bank_pfn_start(bank);
		pfn2 = bank_pfn_end(bank);

		page = pfn_to_page(pfn1);
		end  = pfn_to_page(pfn2 - 1) + 1;

		do {
			total++;
			if (PageReserved(page))
				reserved++;
			else if (PageSwapCache(page))
				cached++;
			else if (PageSlab(page))
				slab++;
			else if (!page_count(page))
				free++;
			else
				shared += page_count(page) - 1;
			page++;
		} while (page < end);
	}

	printk("%d pages of RAM\n", total);
	printk("%d free pages\n", free);
	printk("%d reserved pages\n", reserved);
	printk("%d slab pages\n", slab);
	printk("%d pages shared\n", shared);
	printk("%d pages swap cached\n", cached);
}

static void __init find_limits(unsigned long *min, unsigned long *max_low,
		unsigned long *max_high)
{

	/* +--max_high-----+---------------+ */
	/* |               |               | */
	/* |		   |               | */
	/* |               |          high | */
	/* +--max_low------+---------------+ */
	/* |               |    normal     | */
	/* +---min---------+---------------+ */
	/* |               | DMA           | */
	/* +---------------+---------------+ */

	struct meminfo *mi = &meminfo; //@@ meminfo(전역) - arch/arm/mm/init.c L92
	int i;

	/* This assumes the meminfo array is properly sorted */
	*min = bank_pfn_start(&mi->bank[0]);	//@@ bank[0].start의 PFN(Page Frame Number),
	//@@ bank_pfn_start(): bank->start를 PFN로 변환
	//@@ start가 0x20000000일 경우 PFN = 0x20000

	for_each_bank(i, mi)
		if (mi->bank[i].highmem) //@@ hihgmem 뱅크를 찾음
			break;

	*max_low = bank_pfn_end(&mi->bank[i - 1]); //@@ highmem 앞 뱅크(normal의 마지막 뱅크) end의 PFN
	*max_high = bank_pfn_end(&mi->bank[mi->nr_banks - 1]); //@@ 마지막 뱅크 end의 PFN
}

static void __init arm_bootmem_init(unsigned long start_pfn,
	unsigned long end_pfn)
{
	struct memblock_region *reg;
	unsigned int boot_pages;
	phys_addr_t bitmap;
	pg_data_t *pgdat;

	/*
	 * Allocate the bootmem bitmap page.  This must be in a region
	 * of memory which has already been mapped.
	 */
	//@@ boot_pages: 페이지 프레임의 개수를 비트맵으로 표현하기 위한 바이트수(bytes)를 할당하는데 필요한 페이지 개수
	boot_pages = bootmem_bootmap_pages(end_pfn - start_pfn);
	
	//@@ 2013.11.02 END
	//@@ L1_CACHE_SHIFT = 6
	//@@ L1_CACHE_BYTES = (1 << L1_CACHE_SHIFT)
	//@@ bitmap: (boot_pages << PAGE_SHIFT) 크기 만큼의 물리메모리의 시작주소
	bitmap = memblock_alloc_base(boot_pages << PAGE_SHIFT,	//@@ boot_pages * 4K
							L1_CACHE_BYTES, //@@ L1_CACHE_BYTES = 64
							__pfn_to_phys(end_pfn)); //@@ end_pfn(마지막 normal 뱅크의 PFN)을 물리주소로 변환

	/*
	 * Initialise the bootmem allocator, handing the
	 * memory banks over to bootmem.
	 */
	node_set_online(0); //@@ 0노드를 ONLINE시킴
	pgdat = NODE_DATA(0); //@@ = &contig_page_data(전역)
	
	init_bootmem_node(pgdat, __phys_to_pfn(bitmap), start_pfn, end_pfn); //@@ 부트 메모리를 위한 비트맵을 등록
	//@@ [2013.11.23] 작업 완료

	//@@ [2013.11.30] [19:00-22:00] [START]
	/* Free the lowmem regions from memblock into bootmem. */
	//@@ bootmem의 memblock의 memory.region들의
	//@@ base 주소부터 size에 해당하는 페이지프레임만 bdata->node_bootmem_map에 0으로 설정
	for_each_memblock(memory, reg) { //@@ reg = memblock.memory.regions
		unsigned long start = memblock_region_memory_base_pfn(reg); //@@ 시작 페이지 프레임 넘버
		unsigned long end = memblock_region_memory_end_pfn(reg); //@@ 마지막 페이지 프레임 넘버

		if (end >= end_pfn) //@@ ???
			end = end_pfn;
		if (start >= end) //@@ ???
			break;

		free_bootmem(__pfn_to_phys(start), (end - start) << PAGE_SHIFT); //@@ TODO free_bootmem() 분석
	}

	/* Reserve the lowmem memblock reserved regions in bootmem. */
	for_each_memblock(reserved, reg) {
		unsigned long start = memblock_region_reserved_base_pfn(reg);
		unsigned long end = memblock_region_reserved_end_pfn(reg);

		if (end >= end_pfn)
			end = end_pfn;
		if (start >= end)
			break;

		reserve_bootmem(__pfn_to_phys(start),
			        (end - start) << PAGE_SHIFT, BOOTMEM_DEFAULT);
	}
}

#ifdef CONFIG_ZONE_DMA

unsigned long arm_dma_zone_size __read_mostly;
EXPORT_SYMBOL(arm_dma_zone_size);

/*
 * The DMA mask corresponding to the maximum bus address allocatable
 * using GFP_DMA.  The default here places no restriction on DMA
 * allocations.  This must be the smallest DMA mask in the system,
 * so a successful GFP_DMA allocation will always satisfy this.
 */
phys_addr_t arm_dma_limit;

static void __init arm_adjust_dma_zone(unsigned long *size, unsigned long *hole,
	unsigned long dma_size)
{
	if (size[0] <= dma_size)
		return;

	size[ZONE_NORMAL] = size[0] - dma_size;
	size[ZONE_DMA] = dma_size;
	hole[ZONE_NORMAL] = hole[0];
	hole[ZONE_DMA] = 0;
}
#endif

void __init setup_dma_zone(struct machine_desc *mdesc)
{
	//@@ CONFIG_ZONE_DMA not found
	//@@ arm는에서는 ISA 버스를 사용하지 않기 때문에 zone_dma 를 사용하지 않는다.
#ifdef CONFIG_ZONE_DMA
	if (mdesc->dma_zone_size) {
		arm_dma_zone_size = mdesc->dma_zone_size;
		arm_dma_limit = PHYS_OFFSET + arm_dma_zone_size - 1;
	} else
		arm_dma_limit = 0xffffffff;
#endif
}

static void __init arm_bootmem_free(unsigned long min, unsigned long max_low,
		unsigned long max_high)
{
	unsigned long zone_size[MAX_NR_ZONES], zhole_size[MAX_NR_ZONES];
	struct memblock_region *reg;

	/*
	 * initialise the zones.
	 */
	memset(zone_size, 0, sizeof(zone_size));

	/*
	 * The memory size has already been determined.  If we need
	 * to do anything fancy with the allocation of this memory
	 * to the zones, now is the time to do it.
	 */
	//@@ min: 첫번째 뱅크 start의 PFN
	//@@ max_low: normal의 마지막 뱅크 end의 PFN
	//@@ max_high: 마지막 뱅크 end의 PFN

	zone_size[0] = max_low - min;
#ifdef CONFIG_HIGHMEM
	zone_size[ZONE_HIGHMEM] = max_high - max_low;
#endif

	/*
	 * Calculate the size of the holes.
	 *  holes = node_size - sum(bank_sizes)
	 */
	memcpy(zhole_size, zone_size, sizeof(zhole_size));
	for_each_memblock(memory, reg) {
		unsigned long start = memblock_region_memory_base_pfn(reg);
		unsigned long end = memblock_region_memory_end_pfn(reg);

		if (start < max_low) {
			unsigned long low_end = min(end, max_low);
			zhole_size[0] -= low_end - start;
		}
#ifdef CONFIG_HIGHMEM
		if (end > max_low) {
			unsigned long high_start = max(start, max_low);
			zhole_size[ZONE_HIGHMEM] -= end - high_start;
		}
#endif
	}

#ifdef CONFIG_ZONE_DMA
	/*
	 * Adjust the sizes according to any special requirements for
	 * this machine type.
	 */
	if (arm_dma_zone_size)
		arm_adjust_dma_zone(zone_size, zhole_size,
				arm_dma_zone_size >> PAGE_SHIFT);
#endif

	//@@ 책 2.3 Zone Initialization
	free_area_init_node(0, zone_size, min, zhole_size);
}

#ifdef CONFIG_HAVE_ARCH_PFN_VALID
int pfn_valid(unsigned long pfn)
{
	return memblock_is_memory(__pfn_to_phys(pfn));
}
EXPORT_SYMBOL(pfn_valid);
#endif

#ifndef CONFIG_SPARSEMEM //@@ CONFIG_SPARSEMEM = y
static void __init arm_memory_present(void)
{
}
#else
static void __init arm_memory_present(void)
{
	struct memblock_region *reg;

	for_each_memblock(memory, reg)
		memory_present(0/*nid*/, memblock_region_memory_base_pfn(reg),
			       memblock_region_memory_end_pfn(reg));
}
#endif

static bool arm_memblock_steal_permitted = true;

phys_addr_t __init arm_memblock_steal(phys_addr_t size, phys_addr_t align)
{
	phys_addr_t phys;

	BUG_ON(!arm_memblock_steal_permitted);

	phys = memblock_alloc_base(size, align, MEMBLOCK_ALLOC_ANYWHERE);
	memblock_free(phys, size);
	memblock_remove(phys, size);

	return phys;
}

//@@ meminfo로부터 memblock.memory->region을 만들고,
//@@ kernel text/bss, initrd, swapper_pg_dir, dt내의 reverve영역,
//@@ mdesc->reserve영역, DMB contiguos 부분을 memblock.reserve에 추가한다.
void __init arm_memblock_init(struct meminfo *mi, struct machine_desc *mdesc)
{
	int i;

	for (i = 0; i < mi->nr_banks; i++)
		//@@ region 을 추가하고 합치는 함수 
		//@@ region은 인접한 memory bank끼리 묶어 놓은것.
		//@@ ex) bank0 : 512 ~ 1024, bank1: 1024 ~ 1536 => region cnt :1
		//@@     bank0 : 512 ~ 1024, bank1: 1536 ~ 2048 => region cnt :2
		memblock_add(mi->bank[i].start, mi->bank[i].size);

	/* Register the kernel text, kernel data and initrd with memblock. */
#ifdef CONFIG_XIP_KERNEL
	memblock_reserve(__pa(_sdata), _end - _sdata);
#else
	memblock_reserve(__pa(_stext), _end - _stext); //@@ _end - _stext : kernel의 text와 bss size를 합친 크기
	//@@ text, bss 영역을 reserve 영역으로 설정 
#endif

#ifdef CONFIG_BLK_DEV_INITRD
	//@@ initrd 공간을 reserve함.
	if (phys_initrd_size && // Where is phys_initrd_size?
			!memblock_is_region_memory(phys_initrd_start, phys_initrd_size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx is not a memory region - disabling initrd\n",
				(u64)phys_initrd_start, phys_initrd_size);
		phys_initrd_start = phys_initrd_size = 0;
	}

	if (phys_initrd_size &&
			memblock_is_region_reserved(phys_initrd_start, phys_initrd_size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx overlaps in-use memory region - disabling initrd\n",
				(u64)phys_initrd_start, phys_initrd_size);
		phys_initrd_start = phys_initrd_size = 0;
	}
	if (phys_initrd_size) {
		memblock_reserve(phys_initrd_start, phys_initrd_size); //@@ initrd 영역을 reserve 영역으로 설정
		//@@  bootargs = "root=/dev/ram0 rw ramdisk=8192 initrd=0x41000000,8M console=ttySAC2,115200 init=/linuxrc 가 arch/arm/boot/dts/exynos5250-smdk5250.dts 있음. initrd 의 물리시작주소와 크기가 나와있음 

		/* Now convert initrd to virtual addresses */
		initrd_start = __phys_to_virt(phys_initrd_start);
		initrd_end = initrd_start + phys_initrd_size;
	}
#endif

	arm_mm_memblock_reserve(); //@@ swapper_pg_dir 영역 reserve을 로 설정 
	arm_dt_memblock_reserve(); //@@ device tree 내의 reserve 영역으로 설정된 부분 reserve으로 설정

	/* reserve any platform specific memblock areas */
	if (mdesc->reserve) //@@ machine_desc 구조체 reserve 함수가 정의되어있는지 확인 
		mdesc->reserve();

	//@@ 131012 end.

	/*
	 * reserve memory for DMA contiguos allocations,
	 * must come from DMA area inside low memory
	 */
	dma_contiguous_reserve(min(arm_dma_limit, arm_lowmem_limit));

	arm_memblock_steal_permitted = false;
	memblock_allow_resize();
	memblock_dump_all();
	//@@ 분석 다 햇음 2013.10.19
}

void __init bootmem_init(void)
{
	unsigned long min, max_low, max_high;

	max_low = max_high = 0;

	//@@ 책 2.2.2 Calculating the Size of Zones
	find_limits(&min, &max_low, &max_high);
	//@@ min: 첫번째 뱅크 start의 PFN , min_low_pfn
	//@@ max_low: normal의 마지막 뱅크 end의 PFN, max_low_pfn
	//@@ max_high: 마지막 뱅크 end의 PFN, max_pfn

	arm_bootmem_init(min, max_low); //@@ bootmem 초기화

	/*
	 * Sparsemem tries to allocate bootmem in memory_present(),
	 * so must be done after the fixed reservations
	 */
	arm_memory_present(); //@@ nid == 0 

	/*
	 * sparse_init() needs the bootmem allocator up and running.
	 */
	sparse_init(); //@@ [20131221] page를 관리하는 map을 만들어서 mem_section과 연결..

	/*
	 * Now free the memory - free_area_init_node needs
	 * the sparse mem_map arrays initialized by sparse_init()
	 * for memmap_init_zone(), otherwise all PFNs are invalid.
	 */
	//@@ 책 2.3 Zone Initialization
	arm_bootmem_free(min, max_low, max_high); //@@ [20131221] 볼려다가 끝. 책 p.251

	/*
	 * This doesn't seem to be used by the Linux memory manager any
	 * more, but is used by ll_rw_block.  If we can get rid of it, we
	 * also get rid of some of the stuff above as well.
	 *
	 * Note: max_low_pfn and max_pfn reflect the number of _pages_ in
	 * the system, not the maximum PFN.
	 */
	max_low_pfn = max_low - PHYS_PFN_OFFSET;
	max_pfn = max_high - PHYS_PFN_OFFSET;
}

/*
 * Poison init memory with an undefined instruction (ARM) or a branch to an
 * undefined instruction (Thumb).
 */
static inline void poison_init_mem(void *s, size_t count)
{
	u32 *p = (u32 *)s;
	for (; count != 0; count -= 4)
		*p++ = 0xe7fddef0;
}

static inline void
free_memmap(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *start_pg, *end_pg;
	phys_addr_t pg, pgend;

	/*
	 * Convert start_pfn/end_pfn to a struct page pointer.
	 */
	start_pg = pfn_to_page(start_pfn - 1) + 1;
	end_pg = pfn_to_page(end_pfn - 1) + 1;

	/*
	 * Convert to physical addresses, and
	 * round start upwards and end downwards.
	 */
	pg = PAGE_ALIGN(__pa(start_pg));
	pgend = __pa(end_pg) & PAGE_MASK;

	/*
	 * If there are free pages between these,
	 * free the section of the memmap array.
	 */
	if (pg < pgend)
		free_bootmem(pg, pgend - pg);
}

/*
 * The mem_map array can get very big.  Free the unused area of the memory map.
 */
static void __init free_unused_memmap(struct meminfo *mi)
{
	unsigned long bank_start, prev_bank_end = 0;
	unsigned int i;

	/*
	 * This relies on each bank being in address order.
	 * The banks are sorted previously in bootmem_init().
	 */
	for_each_bank(i, mi) {
		struct membank *bank = &mi->bank[i];

		bank_start = bank_pfn_start(bank);

#ifdef CONFIG_SPARSEMEM
		/*
		 * Take care not to free memmap entries that don't exist
		 * due to SPARSEMEM sections which aren't present.
		 */
		bank_start = min(bank_start,
				 ALIGN(prev_bank_end, PAGES_PER_SECTION));
#else
		/*
		 * Align down here since the VM subsystem insists that the
		 * memmap entries are valid from the bank start aligned to
		 * MAX_ORDER_NR_PAGES.
		 */
		bank_start = round_down(bank_start, MAX_ORDER_NR_PAGES);
#endif
		/*
		 * If we had a previous bank, and there is a space
		 * between the current bank and the previous, free it.
		 */
		if (prev_bank_end && prev_bank_end < bank_start)
			free_memmap(prev_bank_end, bank_start); //@@ bank 사이의 align?

		/*
		 * Align up here since the VM subsystem insists that the
		 * memmap entries are valid from the bank end aligned to
		 * MAX_ORDER_NR_PAGES.
		 */
		prev_bank_end = ALIGN(bank_pfn_end(bank), MAX_ORDER_NR_PAGES);
	}

#ifdef CONFIG_SPARSEMEM
	if (!IS_ALIGNED(prev_bank_end, PAGES_PER_SECTION))
		free_memmap(prev_bank_end,
			    ALIGN(prev_bank_end, PAGES_PER_SECTION));
#endif
}

#ifdef CONFIG_HIGHMEM
static inline void free_area_high(unsigned long pfn, unsigned long end)
{
	for (; pfn < end; pfn++)
		free_highmem_page(pfn_to_page(pfn));
}
#endif

static void __init free_highpages(void)
{
#ifdef CONFIG_HIGHMEM
	unsigned long max_low = max_low_pfn + PHYS_PFN_OFFSET;
	//@@ max_low (CPU address) = max_low_pfn (메모리 주소 공간에서 low memory 의 최대 위치)
	//@@ + PHYS_PFN_OFFSET (메모리가 시작하는 CPU address 위치);
	struct memblock_region *mem, *res;

	/* set highmem page free */
	//@@ memory(global var): struct memblock memblock; 의 memblock->memblock_type memory; 를 가리킴
	//@@ in ./include/linux/memblock.h 
	//@@ memblock_type memory: free한 영역을 의미함.
	//@@ TODO: memory 의 단위에 대해서는 나중에.
	for_each_memblock(memory, mem) { //@@ memory block 의 모든 region 순회
		unsigned long start = memblock_region_memory_base_pfn(mem); //@@ 'mem' region 의 초기 주소(base) 의 pfn 반환
		unsigned long end = memblock_region_memory_end_pfn(mem); //@@ 물리 주소 마지막 pfn 반환 (base+size)

		/* Ignore complete lowmem entries */
		if (end <= max_low)
			continue;

		/* Truncate partial highmem entries */
		if (start < max_low)
			start = max_low;

		/* Find and exclude any reserved regions */
		//@@ reserved: struct memblock -> memblock_type reserved;
		//@@ reserved: memblock 에서 이미 할당 된 블록을 의미함.
		//@@ memory 와 reserved 는 독립적임.
		//@@ region 을 순회하면서, region 사이에 비어있는 영역을 free 시킴.
		for_each_memblock(reserved, res) {
			unsigned long res_start, res_end;

			res_start = memblock_region_reserved_base_pfn(res);
			res_end = memblock_region_reserved_end_pfn(res);

			//@@ memory 안에서 region 위치의 가능한 경우들.
			if (res_end < start)
				continue;
			if (res_start < start)
				res_start = start;
			if (res_start > end)
				res_start = end;
			if (res_end > end)
				res_end = end;
			if (res_start != start)
				free_area_high(start, res_start); //@@ region 앞에 비어 있는 경우 free 하는 함수
			start = res_end;
			if (start == end)
				break;
		}

		/* And now free anything which remains */
		if (start < end)
			//@@ region 뒤에 (그리고 memory 안에) 비어있는 영역을 free 시킴.
			free_area_high(start, end);
	}
#endif
}

/*
 * mem_init() marks the free areas in the mem_map and tells us how much
 * memory is free.  This is done after various parts of the system have
 * claimed their memory after the kernel image.
 */
void __init mem_init(void)
{
#ifdef CONFIG_HAVE_TCM
	/* These pointers are filled in on TCM detection */
	extern u32 dtcm_end;
	extern u32 itcm_end;
#endif

	//@@ max_pfn: 아마도 최대 4gb support
	//@@ max_high = bank_pfn_end(&mi->bank[mi->nr_banks - 1]);	//@@ 마지막 뱅크 end의 PFN
	//@@ max_pfn = max_high - PHYS_PFN_OFFSET
	//@@ PHYS_PFN_OFFSET: The PFN of the first RAM page in the kernel (0x80000000 / 0x1000 -> 0x80000)
	//@@ mem_map: mem_map array의 첫 번째 주소

	max_mapnr   = pfn_to_page(max_pfn + PHYS_PFN_OFFSET) - mem_map;
	//@@ max_mapnr = pfn_to_page(max_high) - mem_map;
	//@@ mem_high 는 물리 메모리의 가장 높은 메모리 주소 
	//@@ pfn_to_page(max_high) 를 통해 mem_map 의 마지막 주소 (struct page) 를 얻음
	//@@ max_mapnr = mem_map 에 mapping 되는 struct page 개수 (최대 map 의 개수를 가지는 변수)

	/* this will put all unused low memory onto the freelists */
	free_unused_memmap(&meminfo); //@@ bank 사이에 연속되지 않은 영역이나 align 이 안된 부분의 mem_map 을 free 함 (free_memmap)
	//@@ 다시 말하면 bank와 bank사이의 bitmap을 free하는데, 이 공간은 사용 못하는 공간일듯 한데
	//@@ 왜 하는 이유는 잘 모르겠다.
	
	//@@ bootmem 으로 할당된 메모리 free 하고,
	//@@ buddy allocator 로 다 초기화 함.
	//@@ bootmem 해제할 때, buddy allocator 의 free_pages 관련 코드를 이용한다.
	//@@ ./mm/page_alloc.c: buddy allocator 관련 코드
	free_all_bootmem(); //@@ [2014.08.23] 완료.

#ifdef CONFIG_SA1111
	/* now that our DMA memory is actually so designated, we can free it */
	free_reserved_area(__va(PHYS_OFFSET), swapper_pg_dir, -1, NULL);
#endif

	//@@ bootmem 으로 관리되지 않았던 high memory 영역을 free 함.
	//@@ high memory 영역은 memblock 으로 관리되었던 영역이고,
	//@@ memory 영역과 reserved 영역을 참고해서 free 함.
	free_highpages();

	mem_init_print_info(NULL);

#define MLK(b, t) b, t, ((t) - (b)) >> 10
#define MLM(b, t) b, t, ((t) - (b)) >> 20
#define MLK_ROUNDUP(b, t) b, t, DIV_ROUND_UP(((t) - (b)), SZ_1K)

	printk(KERN_NOTICE "Virtual kernel memory layout:\n"
			"    vector  : 0x%08lx - 0x%08lx   (%4ld kB)\n"
#ifdef CONFIG_HAVE_TCM
			"    DTCM    : 0x%08lx - 0x%08lx   (%4ld kB)\n"
			"    ITCM    : 0x%08lx - 0x%08lx   (%4ld kB)\n"
#endif
			"    fixmap  : 0x%08lx - 0x%08lx   (%4ld kB)\n"
			"    vmalloc : 0x%08lx - 0x%08lx   (%4ld MB)\n"
			"    lowmem  : 0x%08lx - 0x%08lx   (%4ld MB)\n"
#ifdef CONFIG_HIGHMEM
			"    pkmap   : 0x%08lx - 0x%08lx   (%4ld MB)\n"
#endif
#ifdef CONFIG_MODULES
			"    modules : 0x%08lx - 0x%08lx   (%4ld MB)\n"
#endif
			"      .text : 0x%p" " - 0x%p" "   (%4d kB)\n"
			"      .init : 0x%p" " - 0x%p" "   (%4d kB)\n"
			"      .data : 0x%p" " - 0x%p" "   (%4d kB)\n"
			"       .bss : 0x%p" " - 0x%p" "   (%4d kB)\n",

			MLK(UL(CONFIG_VECTORS_BASE), UL(CONFIG_VECTORS_BASE) +
				(PAGE_SIZE)),
#ifdef CONFIG_HAVE_TCM
			MLK(DTCM_OFFSET, (unsigned long) dtcm_end),
			MLK(ITCM_OFFSET, (unsigned long) itcm_end),
#endif
			MLK(FIXADDR_START, FIXADDR_TOP),
			MLM(VMALLOC_START, VMALLOC_END),
			MLM(PAGE_OFFSET, (unsigned long)high_memory),
#ifdef CONFIG_HIGHMEM
			MLM(PKMAP_BASE, (PKMAP_BASE) + (LAST_PKMAP) *
					(PAGE_SIZE)),
#endif
#ifdef CONFIG_MODULES
			MLM(MODULES_VADDR, MODULES_END),
#endif

			MLK_ROUNDUP(_text, _etext),
			MLK_ROUNDUP(__init_begin, __init_end),
			MLK_ROUNDUP(_sdata, _edata),
			MLK_ROUNDUP(__bss_start, __bss_stop));

#undef MLK
#undef MLM
#undef MLK_ROUNDUP

	/*
	 * Check boundaries twice: Some fundamental inconsistencies can
	 * be detected at build time already.
	 */
#ifdef CONFIG_MMU
	BUILD_BUG_ON(TASK_SIZE				> MODULES_VADDR);
	BUG_ON(TASK_SIZE 				> MODULES_VADDR);
#endif

#ifdef CONFIG_HIGHMEM
	BUILD_BUG_ON(PKMAP_BASE + LAST_PKMAP * PAGE_SIZE > PAGE_OFFSET);
	BUG_ON(PKMAP_BASE + LAST_PKMAP * PAGE_SIZE	> PAGE_OFFSET);
#endif

	if (PAGE_SIZE >= 16384 && get_num_physpages() <= 128) {
		extern int sysctl_overcommit_memory;
		/*
		 * On a machine this small we won't get
		 * anywhere without overcommit, so turn
		 * it on by default.
		 */
		sysctl_overcommit_memory = OVERCOMMIT_ALWAYS;
	}
}

void free_initmem(void)
{
#ifdef CONFIG_HAVE_TCM
	extern char __tcm_start, __tcm_end;

	poison_init_mem(&__tcm_start, &__tcm_end - &__tcm_start);
	free_reserved_area(&__tcm_start, &__tcm_end, -1, "TCM link");
#endif

	poison_init_mem(__init_begin, __init_end - __init_begin);
	if (!machine_is_integrator() && !machine_is_cintegrator())
		free_initmem_default(-1);
}

#ifdef CONFIG_BLK_DEV_INITRD

static int keep_initrd;

void free_initrd_mem(unsigned long start, unsigned long end)
{
	if (!keep_initrd) {
		poison_init_mem((void *)start, PAGE_ALIGN(end) - start);
		free_reserved_area((void *)start, (void *)end, -1, "initrd");
	}
}

static int __init keepinitrd_setup(char *__unused)
{
	keep_initrd = 1;
	return 1;
}

__setup("keepinitrd", keepinitrd_setup);
#endif
