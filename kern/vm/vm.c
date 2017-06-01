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

static struct page_table_entry *page_table_seek(struct addrspace *as, vaddr_t faultaddress) {

        uint32_t hash = hpt_hash(as, faultaddress);

        lock_acquire(page_table_lock);
        struct page_table_entry *entry = page_table[hash];

        while (entry != NULL) {
                if (entry->vaddr == faultaddress && entry->pid == (uint32_t) as) {
                        lock_release(page_table_lock);
                        return entry;
                }
                entry = entry->next;
        }
        lock_release(page_table_lock);
        return NULL;
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
                lock_acquire(page_table_lock);
                struct page_table_entry *cur = page_table[i], *prev = NULL;

                while(cur){
                        if(cur->pid == (uint32_t) as){
                                //TODO decrement
                                if(!prev){
                                        page_table[i] = cur->next;
                                }
                                else{
                                        prev->next = cur->next;
                                }

                                if(cur->elo & PAGE_FRAME){
                                        free_kpages(PADDR_TO_KVADDR(cur->elo & PAGE_FRAME));
                                }
                                kfree(cur);

                                cur = prev ? prev->next : page_table[i];
                        }
                        else{
                                prev = cur;
                                cur = cur->next;
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
                                        lock_release(page_table_lock);
                                        return ENOMEM;
                                }

                                increment_ref_count(cur->elo & PAGE_FRAME);
                                
                                cur->elo &= ~TLBLO_DIRTY;
                                new->elo = cur->elo;

                                new->vaddr = cur->vaddr;
                                new->pid = (uint32_t)newas;

                                int hash = hpt_hash(newas, new->vaddr);
                                new->next = page_table[hash];
                                page_table[hash] = new;
                        }
                }
                lock_release(page_table_lock);
        }

        return 0;
}

static int on_readonly_fault(vaddr_t full_faultaddress) {
        struct addrspace *as = proc_getas();
        as_region region = find_region(as, full_faultaddress);
        if (!region) {
                return EFAULT;
        }

        if(region->writeable) {
                vaddr_t faultaddress = full_faultaddress & PAGE_FRAME;

                struct page_table_entry *entry = page_table_seek(as, faultaddress);
                vaddr_t new = cow(PADDR_TO_KVADDR(entry->elo & PAGE_FRAME));
                entry->elo = KVADDR_TO_PADDR(new) | TLBLO_DIRTY | TLBLO_VALID;
                uint32_t elo = entry->elo;
                
                elo |= as->writeable_mask;

                int spl = splhigh();
                int index = tlb_probe(faultaddress, 0);
                if(index == -1) {
                        //entry was overwritten
                        tlb_random(faultaddress, elo);
                }
                else{
                        tlb_write(faultaddress, elo, index);
                }
                splx(spl);

                return 0;
        }
        else{
                return EFAULT;
        }
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
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

        vaddr_t full_faultaddress = faultaddress;
        faultaddress &= PAGE_FRAME;

        switch (faulttype) {
                case VM_FAULT_READONLY:
                        return on_readonly_fault(full_faultaddress);
                case VM_FAULT_READ:
                case VM_FAULT_WRITE:
                        break;
                default:
                        return EINVAL;
        }

        struct page_table_entry *entry = page_table_seek(as, faultaddress);
        uint32_t elo;

        if (entry) {
                elo = entry->elo;
        }
        else {
                as_region region = find_region(as, full_faultaddress);
                if (!region) {
                        return EFAULT;
                }

                vaddr_t vaddr = alloc_kpages(1);
                if (vaddr == 0) {
                        return ENOMEM;
                }
                bzero((void*) vaddr, PAGE_SIZE);

                struct page_table_entry *new = kmalloc(sizeof(struct page_table_entry));
                if (!new) {
                        free_kpages(vaddr);
                        return ENOMEM;
                }

                new->pid = (uint32_t) as;
                new->vaddr = faultaddress;
                new->elo = KVADDR_TO_PADDR(vaddr) | TLBLO_VALID;
                if (region->writeable) {
                        new->elo |= TLBLO_DIRTY;
                }

                lock_acquire(page_table_lock);
                uint32_t hash = hpt_hash(as, faultaddress);
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

