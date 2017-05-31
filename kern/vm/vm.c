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
        index = (((uint32_t )as) ^ (faultaddr >> PAGE_BITS)) % table_size;
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

void vm_destroy(struct addrspace *as)
{
        size_t i;
        for(i = 0; i < table_size; i++){
                struct page_table_entry *cur, *prev = NULL;

                lock_acquire(page_table_lock);
                for(cur = page_table[i]; cur; prev = cur, cur = cur->next){

                        if(cur->pid == (uint32_t) as){
                                prev->next = cur->next;
                                if(cur->vaddr & PAGE_FRAME){
                                        free_kpages(cur->vaddr & PAGE_FRAME);
                                }
                                kfree(cur);

                                cur = prev;
                        }
                }
                lock_release(page_table_lock);
        }
}

int vm_copy(struct addrspace *old, struct addrspace *newas) 
{
        size_t i;
        for(i = 0; i < table_size; i++){
                struct page_table_entry *cur;

                lock_acquire(page_table_lock);
                for(cur = page_table[i]; cur; cur = cur->next){
                        if(cur->pid == (uint32_t) old){

                                struct page_table_entry *new = kmalloc(sizeof(struct page_table_entry));
                                if(!new){
                                        return ENOMEM;
                                }

                                new->elo = KVADDR_TO_PADDR(alloc_kpages(1));
                                if(!new->elo){
                                        return ENOMEM;
                                }

                                memcpy((void*)PADDR_TO_KVADDR(new->elo), (void*)PADDR_TO_KVADDR(cur->elo & PAGE_FRAME), PAGE_SIZE);

                                new->elo |= (cur->elo & ~PAGE_FRAME);

                                new->vaddr = cur->vaddr;
                                new->pid = (uint32_t)newas;

                                int hash = hpt_hash((struct addrspace *)new->pid, new->vaddr);
                                new->next = page_table[hash];
                                page_table[hash] = new;
                        }
                }
                lock_release(page_table_lock);
        }

        return 0;
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
        vaddr_t full_faultaddress = faultaddress;
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

        bool found = false;

        lock_acquire(page_table_lock);
        while (entry != NULL) {
                if (entry->vaddr == faultaddress && entry->pid == (uint32_t) as) {
                        elo = entry->elo;
                        found = true;
                        break;
                }
                entry = entry->next;
        }
        lock_release(page_table_lock);

        if (found == false) {
                as_region region = find_region(as, full_faultaddress);
                if (!region) {
                        return EFAULT;
                }

                paddr_t paddr = KVADDR_TO_PADDR(alloc_kpages(1));
                if (paddr == 0) {
                        return ENOMEM;
                }
                bzero((void*) PADDR_TO_KVADDR(paddr), PAGE_SIZE);

                struct page_table_entry *new = kmalloc(sizeof(struct page_table_entry));

                new->pid = (uint32_t) as;
                new->vaddr = faultaddress;
                new->elo = paddr | TLBLO_VALID;
                if (region->writeable) {
                        new->elo |= TLBLO_DIRTY;
                }

                lock_acquire(page_table_lock);
                new->next = page_table[hash];
                page_table[hash] = new;
                lock_release(page_table_lock);

                elo = new->elo;
        }

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

