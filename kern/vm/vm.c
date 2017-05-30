#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>

/* Place your page table functions here */
uint32_t hpt_hash(struct addrspace *as, vaddr_t faultaddr);

uint32_t hpt_hash(struct addrspace *as, vaddr_t faultaddr)
{
        uint32_t index;

        // calling ram_getsize() okay?
        index = (((uint32_t )as) ^ (faultaddr >> PAGE_BITS)) % (ram_getsize() * 2);
        return index;
}

void vm_bootstrap(void)
{
        /* Initialise VM sub-system.  You probably want to initialise your 
           frame table here as well.
        */
        frametable_bootstrap();
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
        uint32_t ehi, elo;
        struct addrspace *as;
        int spl;

        faultaddress &= PAGE_FRAME;

        switch (faulttype) {
                case VM_FAULT_READONLY:
                        return EFAULT;
                case VM_FAULT_READ:
                case VM_FAULT_WRITE:
                        break;
                default:
                        return EINVAL;
        }

        if (curproc == NULL) {
                /*
                 * No process. This is probably a kernel fault early
                 * in boot. Return EFAULT so as to panic instead of
                 * getting into an infinite faulting loop.
                 */
                return EFAULT;
        }

        as = proc_getas();
        if (as == NULL) {
                /*
                 * No address space set up. This is probably also a
                 * kernel fault early in boot.
                 */
                return EFAULT;
        }

        uint32_t hash = hpt_hash(as, faultaddress);
        if (page_table[hash].pid == 0) {

        }
        else {
                /* Disable interrupts on this CPU while frobbing the TLB. */
                spl = splhigh();

                ehi = faultaddress;
                elo = page_table[hash].elo;
                tlb_random(ehi, elo);

                splx(spl);
                return 0;
        }

        return EFAULT;
}

/*
 *
 * SMP-specific functions.  Unused in our configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
        (void)ts;
        panic("vm tried to do tlb shootdown?!\n");
}

