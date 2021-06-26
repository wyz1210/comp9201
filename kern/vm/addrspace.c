/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

/* 
 * Address Space
 */

/* allocate a data structure used to keep track of an address space */
struct addrspace *as_create(void)
{
	struct addrspace *as;
	as = (struct addrspace *)kmalloc(sizeof(struct addrspace));
	/* return Null if kmalloc fail  */
	if (as == NULL)
	{
		return NULL;
	}
	bzero(as, sizeof(struct addrspace)); 
	return as;
}

/* 
– allocates a new (destination) address space
– adds all the same regions as source
– roughly, for each mapped page in source

1. allocate a frame in dest
2. copy contents from source frame to dest frame
3. add PT entry for dest
 */
int as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;
	int err;

	newas = as_create();
	if (newas == NULL)
	{
		return ENOMEM;
	}

	/* Here we need to duplicate an address space, and assign it to the 'ret' address */

	/* duplicate the global writable state */
	newas ->writable = old ->writable;

	/* duplicate the page table */
	err = page_table_dup(newas, old);
	if(err){
		return err;
	}

	/* duplicate the region list */
	struct region *old_temp = old->regions;
	newas -> regions = region_dup(old_temp, &err);
	if(err){ return err; }
	struct region *temp = newas->regions;

	while(old_temp->next != NULL){
		temp->next = region_dup(old_temp->next, &err);
		if(err){
			/* free the memory already assigned */
			region_destroy(newas->regions);
			return err;
		}
		temp = temp->next;
		old_temp= old_temp->next;
	}
	temp -> next = NULL;
	
	*ret = newas;
	return 0;
}


/* 
– deallocate book keeping and page tables.
* deallocate frames used
 */
void as_destroy(struct addrspace *as)
{
	if(as == NULL){
		return;
	}

	page_table_destroy(as);
	region_destroy(as -> regions);

	kfree(as);
}

/* flush TLB */
/* (or set the hardware asid) No required */
void as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

/* flush TLB */
/* – (or flush an asid) Not required */
void as_deactivate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */

/* usually implemented as a linked list of region specifications */
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize, int readable, int writeable, int executable)
{
	int err = 0;
	struct region *new_region = NULL;
	uint8_t permission = 0;
	new_region = (struct region*)kmalloc(sizeof(struct region));
	if(!new_region){
		return ENOMEM;
	}

	permission = executable | readable | writeable;

	new_region -> permission = permission; 
	new_region -> size = memsize; 
	new_region -> addr_start = vaddr;
	new_region -> next = NULL; 

	if(as -> regions == NULL){
		as ->regions = new_region; 
	}else{
		struct region *temp = as ->regions;
		while(temp -> next != NULL){
			err = region_checkInUse(temp, new_region); 
			if(err){
				kfree(new_region); 
				return err;
			}
			temp = temp -> next; 
		}
		/* insert the new region to the tail of region list */
		temp ->next = new_region;
	}

	return 0; 
}

/* make READONLY regions READWRITE for loading purposes */
int as_prepare_load(struct addrspace *as)
{	
	if(as == NULL){
		return ENOMEM; 
	}
	as -> writable = 1; 
	return 0;
}

/* enforce READONLY again */
int as_complete_load(struct addrspace *as)
{
	if(as == NULL){
		return ENOMEM;
	}
	as -> writable = 0; 
	return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	int err; 
	if(as == NULL){	
		return ENOMEM; 
	}

	/* stack space memory should be readable and writable */
	err = as_define_region(as, USERSTACK - 16 * PAGE_SIZE, 16 * PAGE_SIZE, 1 << 2, 1 << 1, 0);
	if(err){return err;}

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}

// 8  6  6 
// l1 l2 l3 12bit 
/* search the page table and acquire the entry corresponding the passed in address */
uint32_t page_table_lookup(struct addrspace* as, vaddr_t addr){
	uint32_t l1 = get_l1_index(addr);
	uint32_t l2 = get_l2_index(addr);
	uint32_t l3 = get_l3_index(addr);
	struct addrspace_l3 *l3_ptr = NULL; 
	
	if(!as->page_table[l1]){
		return 0x0; 
	}

	l3_ptr = as -> page_table[l1] + l2; 
	if(l3_ptr -> entries == NULL){
		return 0x0; 
	}

	return as->page_table[l1][l2].entries[l3];
}

int page_table_dup(struct addrspace *new, struct addrspace *old){	
	int err = 0;
	uint32_t new_frame;

	/* l1 duplicate */
	for(int l1 = 0; l1 < PAGE_L1_NUM; ++l1){
		if(old -> page_table[l1] != NULL){
			err = page_table_l2_init(new, l1);
			if(err){
				return err;
			}

			/* l2 duplicate */
			for(int l2 = 0; l2 < PAGE_L2_L3_NUM; ++l2){
				if(old -> page_table[l1][l2].entries != NULL){
					err = page_table_l3_init(new, l1, l2);
					if(err){
						return err; 
					}

					/* l3 duplicate */
					for(int l3 = 0; l3 < PAGE_L2_L3_NUM; ++l3){
						if(old -> page_table[l1][l2].entries[l3] != 0x0){
							uint32_t old_entry = old -> page_table[l1][l2].entries[l3];

							/* old frame adress */
							uint32_t old_frame = PADDR_TO_KVADDR(old -> page_table[l1][l2].entries[l3] & PAGE_FRAME);

							/* apply for a new page from the physical memory and get the new frame, but note that the new frame address is not a entry as it at kernel segment, we need to map it to userland address and set dirty bit and valid bit later */
							new_frame = alloc_kpages(1);
							if(new_frame == 0x0){
								return ENOMEM;
							}
							
							/* copy content to the new frame */ 
							memmove((void *)new_frame, (void *)old_frame, PAGE_SIZE);

							/* map new address to userland address */
							uint32_t entrylo = KVADDR_TO_PADDR(new_frame);

							/* set the dirty bit and valid bit */
							entrylo |= (old_entry & TLBLO_DIRTY);
							entrylo |= (old_entry & TLBLO_VALID);
						
							new -> page_table[l1][l2].entries[l3] = entrylo;
						}
					}
				}
			}
		}
	}

	return 0;
}

/* insert a entry to the page table */
int page_table_insert(struct addrspace* as, vaddr_t addr, paddr_t entrylo){
	// TODO
	uint32_t l1 = get_l1_index(addr);
	uint32_t l2 = get_l2_index(addr);
	uint32_t l3 = get_l3_index(addr);
	int err = 0; 
	if(!as -> page_table[l1]){
		err = page_table_l2_init(as, l1); 
		if(err){
			return err; 
		}
	}

	if(!as -> page_table[l1][l2].entries){
		err = page_table_l3_init(as, l1, l2); 
		if(err){
			return err; 
		}
	}
	as -> page_table[l1][l2].entries[l3] = entrylo; 
	return 0; 
}

/* Initialize the level2 page table */
int page_table_l2_init(struct addrspace *as, uint32_t l1){
	if(l1 >= PAGE_L1_NUM){
		return 1; 
	}
	as->page_table[l1] = (struct addrspace_l3*)kmalloc(sizeof(struct addrspace_l3) * PAGE_L2_L3_NUM);
	if(as->page_table[l1] == NULL){
		return ENOMEM; 
	}

	for (size_t i = 0; i < PAGE_L2_L3_NUM; ++i)
	{
		as->page_table[l1][i].entries = NULL; 
	}
	// bzero(as->page_table[l1], sizeof(struct addrspace_l3) * PAGE_L2_L3_NUM);

	return 0; 
}

/* Initialize the level 3 page table */
int page_table_l3_init(struct addrspace *as, uint32_t l1, uint32_t l2){
	int err = 0; 
	if(l1 >= PAGE_L1_NUM || l2 > PAGE_L2_L3_NUM){
		return 1; 
	}

	if(!as->page_table[l1]){
		err = page_table_l2_init(as, l1);
		if(err){
			return err; 
		}
	}

	if(as->page_table[l1][l2].entries!=NULL){
		// the l3 entries already exist 
		return err; 
	}
	as->page_table[l1][l2].entries = (uint32_t*)kmalloc(sizeof(uint32_t) * PAGE_L2_L3_NUM);
	if(!as->page_table[l1][l2].entries){
		return ENOMEM; 
	}	

	for (size_t i = 0; i < PAGE_L2_L3_NUM; ++i)
	{
		as->page_table[l1][l2].entries[i] = 0x0; 
	}
	return err; 
}

/* destroy the page table */
void page_table_destroy(struct addrspace* as){
	for (size_t i = 0; i < PAGE_L1_NUM; i++){
		if(as->page_table[i]!=NULL){
			for (size_t j=0; j < PAGE_L2_L3_NUM; j++){
				if(as->page_table[i][j].entries!=NULL){
					for (size_t k=0; k < PAGE_L2_L3_NUM; k++){
						uint32_t* addr = as->page_table[i][j].entries + k;
						if(*addr!= 0x0){
							free_kpages(PADDR_TO_KVADDR(*addr));
						}
					}
					kfree(as->page_table[i][j].entries); 
					as->page_table[i][j].entries = NULL; 
				}
				/* wrong here, as->page_table[i] is assigned memory only once, so here cannot be free sperately  */
				// kfree(as->page_table[i] + j);  
			}
			kfree(as->page_table[i]);
		}
		as -> page_table[i] = NULL;
	}
}


/* 
 * Region
 */

/* destroy a region linked list */
int region_destroy(struct region *ls){
	if(ls == NULL){
		return 0;
	}
	region_destroy(ls -> next);
	kfree(ls);
	return 0;
}

/* duplicate a region node, return error code in the second argument */
struct region *region_dup(struct region *ls, int *ret){
	if(ls == NULL){
		*ret = 0;
		return NULL;
	}

	struct region *head = (struct region *)kmalloc(sizeof(struct region));
	if(head == NULL){
		return NULL;
		*ret = ENOMEM;
	}

	/* copy the attribute */
	head->permission = ls ->permission;
	head->size = ls->size;
	head->addr_start = ls -> addr_start;
	head->next = NULL;
	*ret = 0;
	return head;
}

/* dup the permission to the page according to the region perimission */
uint32_t make_pte(struct region* reg, uint32_t page, uint8_t global_writable){
	/* if during  */
	if(reg->permission & WRITE || global_writable){
		page |= TLBLO_DIRTY;
	}

	if(reg -> permission){
		page |= TLBLO_VALID;
	}
	return page;
}

/* Lookup the region linked list and find out the node contain the required address */
struct region *regions_lookup(struct addrspace *as, vaddr_t addr){
	struct region *cur = as -> regions;
	while(cur != NULL){
		if(cur ->addr_start <= addr && (cur -> addr_start + cur -> size) > addr){
			break;
		}
		cur = cur ->next;
	}
	return cur;
}

int region_checkInUse(struct region *cur, struct region *new){
	uint32_t cur_start = cur -> addr_start; 
	uint32_t cur_end = cur -> addr_start + cur -> size; 

	uint32_t new_start = new -> addr_start; 
	uint32_t new_end = new -> addr_start + new -> size; 
	
	if(
		(cur_start > new_start && cur_start <= new_end) || 
		(cur_end > new_start && cur_end <= new_end)
		){
			return EADDRINUSE; 
		} 
	return 0;
}

/* 
 * Bitwise operation	
 */
uint32_t get_l1_index(vaddr_t addr){
	return addr >> 24; 
}

uint32_t get_l2_index(vaddr_t addr){
	return (addr << 8) >> 26; 
}

uint32_t get_l3_index(vaddr_t addr){
	return (addr << 14) >> 26; 
}
