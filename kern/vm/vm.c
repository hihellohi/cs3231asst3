#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <proc.h>
#include <synch.h>
#include <current.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>

/* Place your page table functions here */
static struct lock *page_table_lock;

static uint32_t hpt_hash(struct addrspace *as, vaddr_t faultaddr)
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
        page_table_lock = lock_create("page_table_lock");
}

static as_region find_region(struct addrspace *as, vaddr_t faultaddress)
{
        for (as_region region = as->first_region; region; region = region->next) {
                if (region->vbase <= faultaddress && (region->vbase + region->size) > faultaddress) {
                        return region;
                }
        }
        return NULL;
}

int vm_copy(struct addrspace *old, struct addrspace *newas) 
{
        (void)old;
        (void)newas;
        return 0;
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

        lock_acquire(page_table_lock);
        struct page_table_entry *entry = page_table[hash];
        struct page_table_entry *prev = NULL;

        bool found = false;
        while (entry != NULL) {
                if (entry->vaddr == faultaddress && entry->pid == (uint32_t) as) {
                        elo = entry->elo;
                        found = true;
                        break;
                }
                prev = entry;
                entry = entry->next;
        }

        if (found == false) {
                as_region region = find_region(as, faultaddress);
                if (!region) {
                        lock_release(page_table_lock);
                        return EFAULT;
                }

                paddr_t paddr = KVADDR_TO_PADDR(alloc_kpages(1));
                if (paddr == 0) {
                        lock_release(page_table_lock);
                        return ENOMEM;
                }
                bzero((void*) PADDR_TO_KVADDR(paddr), PAGE_SIZE);

                struct page_table_entry *new = kmalloc(sizeof(struct page_table_entry));
                if (prev == NULL) {
                        page_table[hash] = new;
                }
                else {
                        prev->next = new;
                }
                new->pid = (uint32_t) as;
                new->vaddr = faultaddress;
                new->next = NULL;
                new->elo = paddr | TLBLO_VALID;
                if (region->writeable) {
                        new->elo |= TLBLO_DIRTY;
                }
                elo = new->elo;
        }
        lock_release(page_table_lock);

        elo |= as->writeable_mask;
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

