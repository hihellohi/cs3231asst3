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

        struct addrspace *as = proc_getas();
        if (as == NULL) {
                /*
                 * No address space set up. This is probably also a
                 * kernel fault early in boot.
                 */
                return EFAULT;
        }

        uint32_t elo;
        uint32_t hash = hpt_hash(as, faultaddress);
        struct page_table_entry *entry = page_table[hash];
        struct page_table_entry *prev = NULL;

        bool found = false;
        while (entry != NULL) {
                if (entry->vaddr == faultaddress) {
                        elo = entry->elo;
                        found = true;
                        break;
                }
                prev = entry;
                entry = entry->next;
        }

        if (found == false) {
                struct page_table_entry *new = kmalloc(sizeof(struct page_table_entry));
                if (prev == NULL) {
                        page_table[hash] = new;
                }
                else {
                        prev->next = new;
                }
                paddr_t paddr = KVADDR_TO_PADDR(alloc_kpages(1));
                if (paddr == 0) {
                        return ENOMEM;
                }
                new->pid = (uint32_t) as;
                new->vaddr = faultaddress;
                new->next = NULL;
                new->elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
                elo = new->elo;
        }

        int spl = splhigh();
        tlb_random(faultaddress, elo);
        splx(spl);

        return 0;
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

