#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <spl.h>
#include <proc.h>

/* Place your page table functions here */

void vm_bootstrap(void)
{
    /* Initialise any global components of your VM sub-system here.  
     *  
     * You may or may not need to add anything here depending what's
     * provided or required by the assignment spec.
     */
}

int check_faulttype(int faulttype){
    if(faulttype == VM_FAULT_READONLY){
        // Attempt to write to read only address space
        return EFAULT;
    }
    if(faulttype != VM_FAULT_READ && faulttype != VM_FAULT_WRITE){
        return EINVAL; 
    }
    return 0;
}

int vm_fault(int faulttype, vaddr_t faultaddress)
{
    int spl;
    struct region *cur; 
    struct addrspace *as;
    uint32_t entrylo = 0x0; 
    uint32_t entryhi = faultaddress & TLBHI_VPAGE; 
    int err = 0;

    /* Check if the fault address is valid */
    if(faultaddress == 0x0 || faultaddress >= 0x80000000){
        return EFAULT;
    }

    /* Check if the faulttype is valid */
    err = check_faulttype(faulttype);
    if(err) {
        return err; 
    }

    /* Check if process is available */
    as = proc_getas();
    if(as == NULL){
        return ENOMEM; 
    }

    entrylo = page_table_lookup(as, faultaddress); 

    /* Translation exist and vaild */
    if(entrylo & TLBLO_VALID){
        /* write not permitted under this address */
        // ???????
        if((faulttype == VM_FAULT_WRITE) && ((entrylo & TLBLO_DIRTY) == 0)) {
            return EFAULT; 
        }

        /* turn off the interrupts */
        spl = splhigh();
        /* then insert into tlb */        
        tlb_random(entryhi, entrylo);
        splx(spl); 

        /* return successfully */
        return 0;
    }

    /* No valid translation */
    cur = regions_lookup(as, faultaddress);
    // if( cur == NULL || cur -> permission == -1){
    if( cur == NULL){
        /* the addr is not held by the current proccess */
        return EFAULT; 
    }

    /* allocate a new page for user */
    uint32_t newpage = alloc_kpages(1);

    /* Out of memory */
    if(newpage == 0){   
        return ENOMEM;
    }

    /* Newly allocated user-level pages are expected to be zero-filled */
    bzero((void *)newpage, PAGE_SIZE);

    /* Map the kseg address to frame address */
    entrylo = KVADDR_TO_PADDR(newpage);

    /* modify the entrylo according to region permission */
    entrylo = make_pte(cur, entrylo, as -> writable);

    /* Insert page into page table */
    err = page_table_insert(as, entryhi, entrylo);

    /* if error, then free the page */
    if(err){
        free_kpages(newpage);
        return err; 
    }
    
    /* turn off the interrupts */
    spl = splhigh();
    tlb_random(entryhi, entrylo);
    splx(spl);
    
    return 0;
}

/*
 * SMP-specific functions.  Unused in our UNSW configuration.
 */

void vm_tlbshootdown(const struct tlbshootdown *ts)
{
    (void)ts;
    panic("vm tried to do tlb shootdown?!\n");
}
