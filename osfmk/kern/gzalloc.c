/*
 * Copyright (c) 2000-2012 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 *	File:	kern/gzalloc.c
 *	Author:	Derek Kumar
 *
 *	"Guard mode" zone allocator, used to trap use-after-free errors,
 *	overruns, underruns, mismatched allocations/frees, uninitialized
 *	zone element use, timing dependent races etc.
 *
 *	The allocator is configured by these boot-args:
 *	gzalloc_size=<size>: target all zones with elements of <size> bytes
 *	gzalloc_min=<size>: target zones with elements >= size
 *	gzalloc_max=<size>: target zones with elements <= size
 * 	gzalloc_min/max can be specified in conjunction to target a range of
 *	sizes
 *	gzalloc_fc_size=<size>: number of zone elements (effectively page
 *	multiple sized) to retain in the free VA cache. This cache is evicted
 *	(backing pages and VA released) in a least-recently-freed fashion.
 *	Larger free VA caches allow for a longer window of opportunity to trap
 *	delayed use-after-free operations, but use more memory.
 *	-gzalloc_wp: Write protect, rather than unmap, freed allocations
 *	lingering in the free VA cache. Useful to disambiguate between
 *	read-after-frees/read overruns and writes. Also permits direct inspection
 *	of the freed element in the cache via the kernel debugger. As each
 *	element has a "header" (trailer in underflow detection mode), the zone
 *	of origin of the element can be easily determined in this mode.
 *	-gzalloc_uf_mode: Underflow detection mode, where the guard page
 *	adjoining each element is placed *before* the element page rather than
 *	after. The element is also located at the top of the page, rather than
 *	abutting the bottom as with the standard overflow detection mode.
 *	-gzalloc_noconsistency: disable consistency checks that flag mismatched
 *	frees, corruptions of the header/trailer signatures etc.
 *	-nogzalloc_mode: Disables the guard mode allocator. The DEBUG kernel
 *	enables the guard allocator for zones sized 8K-16K (if present) by
 *	default, this option can disable that behaviour.
 */

#include <zone_debug.h>
#include <zone_alias_addr.h>

#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <mach/kern_return.h>
#include <mach/machine/vm_types.h>
#include <mach_debug/zone_info.h>
#include <mach/vm_map.h>

#include <kern/kern_types.h>
#include <kern/assert.h>
#include <kern/sched.h>
#include <kern/locks.h>
#include <kern/misc_protos.h>
#include <kern/zalloc.h>
#include <kern/kalloc.h>

#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>

#include <pexpert/pexpert.h>

#include <machine/machparam.h>

#include <libkern/OSDebug.h>
#include <libkern/OSAtomic.h>
#include <sys/kdebug.h>

extern boolean_t vm_kernel_ready, kmem_ready;
boolean_t gzalloc_mode = FALSE;
uint32_t pdzalloc_count, pdzfree_count;

#define	GZALLOC_MIN_DEFAULT (1024)
#define GZDEADZONE ((zone_t) 0xDEAD201E)
#define GZALLOC_SIGNATURE (0xABADCAFE)
#define GZALLOC_RESERVE_SIZE_DEFAULT (2 * 1024 * 1024)
#define GZFC_DEFAULT_SIZE (1024)

char gzalloc_fill_pattern = 0x67; /* 'g' */

uint32_t gzalloc_min = ~0U;
uint32_t gzalloc_max = 0;
uint32_t gzalloc_size = 0;
uint64_t gzalloc_allocated, gzalloc_freed, gzalloc_early_alloc, gzalloc_early_free, gzalloc_wasted;
boolean_t gzalloc_uf_mode = FALSE, gzalloc_consistency_checks = TRUE;
vm_prot_t gzalloc_prot = VM_PROT_NONE;
uint32_t gzalloc_guard = KMA_GUARD_LAST;
uint32_t gzfc_size = GZFC_DEFAULT_SIZE;

vm_map_t gzalloc_map;
vm_offset_t gzalloc_map_min, gzalloc_map_max;
vm_offset_t gzalloc_reserve;
vm_size_t gzalloc_reserve_size;

typedef struct gzalloc_header {
	zone_t gzone;
	uint32_t  gzsize;
	uint32_t  gzsig;
} gzhdr_t;

#define GZHEADER_SIZE (sizeof(gzhdr_t))

extern zone_t vm_page_zone;

void gzalloc_reconfigure(__unused zone_t z) {
	/* Nothing for now */
}

boolean_t gzalloc_enabled(void) {
	return gzalloc_mode;
}

void gzalloc_zone_init(zone_t z) {
	if (gzalloc_mode) {
		bzero(&z->gz, sizeof(z->gz));

		if (gzfc_size && (z->elem_size >= gzalloc_min) && (z->elem_size <= gzalloc_max) && (z->gzalloc_exempt == FALSE)) {
			vm_size_t gzfcsz = round_page(sizeof(*z->gz.gzfc) * gzfc_size);

			/* If the VM/kmem system aren't yet configured, carve
			 * out the free element cache structure directly from the
			 * gzalloc_reserve supplied by the pmap layer.
			*/
			if (!kmem_ready) {
				if (gzalloc_reserve_size < gzfcsz)
					panic("gzalloc reserve exhausted");

				z->gz.gzfc = (vm_offset_t *)gzalloc_reserve;
				gzalloc_reserve += gzfcsz;
				gzalloc_reserve_size -= gzfcsz;
			} else {
				kern_return_t kr;

				if ((kr = kernel_memory_allocate(kernel_map, (vm_offset_t *)&z->gz.gzfc, gzfcsz, 0, KMA_KOBJECT)) != KERN_SUCCESS) {
					panic("zinit/gzalloc: kernel_memory_allocate failed (%d) for 0x%lx bytes", kr, (unsigned long) gzfcsz);
				}
			}
			bzero((void *)z->gz.gzfc, gzfcsz);
		}
	}
}

void gzalloc_configure(void) {
	char temp_buf[16];

	if (PE_parse_boot_argn("-gzalloc_mode", temp_buf, sizeof (temp_buf))) {
		gzalloc_mode = TRUE;
		gzalloc_min = GZALLOC_MIN_DEFAULT;
#if	ZONE_DEBUG		
		gzalloc_min += (typeof(gzalloc_min))ZONE_DEBUG_OFFSET;
#endif
		gzalloc_max = ~0U;
	}

	if (PE_parse_boot_argn("gzalloc_min", &gzalloc_min, sizeof(gzalloc_min))) {
#if	ZONE_DEBUG		
		gzalloc_min += (typeof(gzalloc_min))ZONE_DEBUG_OFFSET;
#endif
		gzalloc_mode = TRUE;
		gzalloc_max = ~0U;
	}

	if (PE_parse_boot_argn("gzalloc_max", &gzalloc_max, sizeof(gzalloc_max))) {
#if	ZONE_DEBUG		
		gzalloc_max += (typeof(gzalloc_min))ZONE_DEBUG_OFFSET;
#endif
		gzalloc_mode = TRUE;
		if (gzalloc_min == ~0U)
			gzalloc_min = 0;
	}

	if (PE_parse_boot_argn("gzalloc_size", &gzalloc_size, sizeof(gzalloc_size))) {
#if	ZONE_DEBUG		
		gzalloc_size += (typeof(gzalloc_min))ZONE_DEBUG_OFFSET;
#endif
		gzalloc_min = gzalloc_max = gzalloc_size;
		gzalloc_mode = TRUE;
	}

	(void)PE_parse_boot_argn("gzalloc_fc_size", &gzfc_size, sizeof(gzfc_size));

	if (PE_parse_boot_argn("-gzalloc_wp", temp_buf, sizeof (temp_buf))) {
		gzalloc_prot = VM_PROT_READ;
	}

	if (PE_parse_boot_argn("-gzalloc_uf_mode", temp_buf, sizeof (temp_buf))) {
		gzalloc_uf_mode = TRUE;
		gzalloc_guard = KMA_GUARD_FIRST;
	}

	if (PE_parse_boot_argn("-gzalloc_noconsistency", temp_buf, sizeof (temp_buf))) {
		gzalloc_consistency_checks = FALSE;
	}
#if	DEBUG
	if (gzalloc_mode == FALSE) {
		gzalloc_min = 8192;
		gzalloc_max = 16384;
		gzalloc_prot = VM_PROT_READ;
		gzalloc_mode = TRUE;
	}
#endif
	if (PE_parse_boot_argn("-nogzalloc_mode", temp_buf, sizeof (temp_buf)))
		gzalloc_mode = FALSE;

	if (gzalloc_mode) {
		gzalloc_reserve_size = GZALLOC_RESERVE_SIZE_DEFAULT;
		gzalloc_reserve = (vm_offset_t) pmap_steal_memory(gzalloc_reserve_size);
	}
}

void gzalloc_init(vm_size_t max_zonemap_size) {
	kern_return_t retval;

	if (gzalloc_mode) {
		retval = kmem_suballoc(kernel_map, &gzalloc_map_min, (max_zonemap_size << 2),
		    FALSE, VM_FLAGS_ANYWHERE | VM_FLAGS_PERMANENT,
		    &gzalloc_map);
	
		if (retval != KERN_SUCCESS)
			panic("zone_init: kmem_suballoc(gzalloc) failed");
		gzalloc_map_max = gzalloc_map_min + (max_zonemap_size << 2);
	}
}

vm_offset_t
gzalloc_alloc(zone_t zone, boolean_t canblock) {
	vm_offset_t addr = 0;

	if (__improbable(gzalloc_mode &&
		(((zone->elem_size >= gzalloc_min) &&
		    (zone->elem_size <= gzalloc_max))) &&
		(zone->gzalloc_exempt == 0))) {

		if (get_preemption_level() != 0) {
			if (canblock == TRUE) {
				pdzalloc_count++;
			}
			else
				return 0;
		}

		vm_offset_t rounded_size = round_page(zone->elem_size + GZHEADER_SIZE);
		vm_offset_t residue = rounded_size - zone->elem_size;
		vm_offset_t gzaddr = 0;
		gzhdr_t *gzh;

		if (!kmem_ready || (vm_page_zone == ZONE_NULL)) {
			/* Early allocations are supplied directly from the
			 * reserve.
			 */
			if (gzalloc_reserve_size < rounded_size)
				panic("gzalloc reserve exhausted");
			gzaddr = gzalloc_reserve;
			/* No guard page for these early allocations, just
			 * waste an additional page.
			 */
			gzalloc_reserve += rounded_size + PAGE_SIZE;
			gzalloc_reserve_size -= rounded_size + PAGE_SIZE;
			OSAddAtomic64((SInt32) (rounded_size), &gzalloc_early_alloc);
		}
		else {
			kern_return_t kr = kernel_memory_allocate(gzalloc_map,
			    &gzaddr, rounded_size + (1*PAGE_SIZE),
			    0, KMA_KOBJECT | gzalloc_guard);
			if (kr != KERN_SUCCESS)
				panic("gzalloc: kernel_memory_allocate for size 0x%llx failed with %d", (uint64_t)rounded_size, kr);

		}

		if (gzalloc_uf_mode) {
			gzaddr += PAGE_SIZE;
			/* The "header" becomes a "footer" in underflow
			 * mode.
			 */
			gzh = (gzhdr_t *) (gzaddr + zone->elem_size);
			addr = gzaddr;
		} else {
			gzh = (gzhdr_t *) (gzaddr + residue - GZHEADER_SIZE);
			addr = (gzaddr + residue);
		}

		/* Fill with a pattern on allocation to trap uninitialized
		 * data use. Since the element size may be "rounded up"
		 * by higher layers such as the kalloc layer, this may
		 * also identify overruns between the originally requested
		 * size and the rounded size via visual inspection.
		 * TBD: plumb through the originally requested size,
		 * prior to rounding by kalloc/IOMalloc etc.
		 * We also add a signature and the zone of origin in a header
		 * prefixed to the allocation.
		 */
		memset((void *)gzaddr, gzalloc_fill_pattern, rounded_size);

		gzh->gzone = (kmem_ready && vm_page_zone) ? zone : GZDEADZONE;
		gzh->gzsize = (uint32_t) zone->elem_size;
		gzh->gzsig = GZALLOC_SIGNATURE;

		lock_zone(zone);
		zone->count++;
		zone->sum_count++;
		zone->cur_size += rounded_size;
		unlock_zone(zone);

		OSAddAtomic64((SInt32) rounded_size, &gzalloc_allocated);
		OSAddAtomic64((SInt32) (rounded_size - zone->elem_size), &gzalloc_wasted);
	}
	return addr;
}

boolean_t gzalloc_free(zone_t zone, void *addr) {
	boolean_t gzfreed = FALSE;
	kern_return_t kr;

	if (__improbable(gzalloc_mode &&
		(((zone->elem_size >= gzalloc_min) &&
		    (zone->elem_size <= gzalloc_max))) &&
		(zone->gzalloc_exempt == 0))) {
		gzhdr_t *gzh;
		vm_offset_t rounded_size = round_page(zone->elem_size + GZHEADER_SIZE);
		vm_offset_t residue = rounded_size - zone->elem_size;
		vm_offset_t saddr;
		vm_offset_t free_addr = 0;

		if (gzalloc_uf_mode) {
			gzh = (gzhdr_t *)((vm_offset_t)addr + zone->elem_size);
			saddr = (vm_offset_t) addr - PAGE_SIZE;
		} else {
			gzh = (gzhdr_t *)((vm_offset_t)addr - GZHEADER_SIZE);
			saddr = ((vm_offset_t)addr) - residue;
		}

		assert((saddr & PAGE_MASK) == 0);

		if (gzalloc_consistency_checks) {
			if (gzh->gzsig != GZALLOC_SIGNATURE) {
				panic("GZALLOC signature mismatch for element %p, expected 0x%x, found 0x%x", addr, GZALLOC_SIGNATURE, gzh->gzsig);
			}

			if (gzh->gzone != zone && (gzh->gzone != GZDEADZONE))
				panic("%s: Mismatched zone or under/overflow, current zone: %p, recorded zone: %p, address: %p", __FUNCTION__, zone, gzh->gzone, (void *)addr);
			/* Partially redundant given the zone check, but may flag header corruption */
			if (gzh->gzsize != zone->elem_size) {
				panic("Mismatched zfree or under/overflow for zone %p, recorded size: 0x%x, element size: 0x%x, address: %p\n", zone, gzh->gzsize, (uint32_t) zone->elem_size, (void *)addr);
			}
		}

		if (!kmem_ready || gzh->gzone == GZDEADZONE) {
			/* For now, just leak frees of early allocations
			 * performed before kmem is fully configured.
			 * They don't seem to get freed currently;
			 * consider ml_static_mfree in the future.
			 */
			OSAddAtomic64((SInt32) (rounded_size), &gzalloc_early_free);
			return TRUE;
		}

		if (get_preemption_level() != 0) {
				pdzfree_count++;
		}

		if (gzfc_size) {
			/* Either write protect or unmap the newly freed
			 * allocation
			 */
			kr = vm_map_protect(
				gzalloc_map,
				saddr,
				saddr + rounded_size + (1 * PAGE_SIZE),
				gzalloc_prot,
				FALSE);
			if (kr != KERN_SUCCESS)
				panic("%s: vm_map_protect: %p, 0x%x", __FUNCTION__, (void *)saddr, kr);
		} else {
			free_addr = saddr;
		}

		lock_zone(zone);

		/* Insert newly freed element into the protected free element
		 * cache, and rotate out the LRU element.
		 */
		if (gzfc_size) {
			if (zone->gz.gzfc_index >= gzfc_size) {
				zone->gz.gzfc_index = 0;
			}
			free_addr = zone->gz.gzfc[zone->gz.gzfc_index];
			zone->gz.gzfc[zone->gz.gzfc_index++] = saddr;
		}

		if (free_addr) {
			zone->count--;
			zone->cur_size -= rounded_size;
		}

		unlock_zone(zone);

		if (free_addr) {
			kr = vm_map_remove(
				gzalloc_map,
				free_addr,
				free_addr + rounded_size + (1 * PAGE_SIZE),
				VM_MAP_REMOVE_KUNWIRE);
			if (kr != KERN_SUCCESS)
				panic("gzfree: vm_map_remove: %p, 0x%x", (void *)free_addr, kr);

			OSAddAtomic64((SInt32)rounded_size, &gzalloc_freed);
			OSAddAtomic64(-((SInt32) (rounded_size - zone->elem_size)), &gzalloc_wasted);
		}

		gzfreed = TRUE;
	}
	return gzfreed;
}
