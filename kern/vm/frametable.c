#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>

/* Place your frametable data-structures here 
 * You probably also want to write a frametable initialisation
 * function and call it from vm_bootstrap
 */
struct frame_table_entry *frame_table = NULL;
struct frame_table_entry *next_free = NULL;

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

void frametable_bootstrap(void) {
        paddr_t top_of_ram = ram_getsize();
        paddr_t location = top_of_ram - (top_of_ram / PAGE_SIZE * sizeof(struct frame_table_entry));
        frame_table = (struct frame_table_entry *) location;
        unsigned table_size = top_of_ram / PAGE_SIZE;

        // mark memory used by frame_table as used
        // location / PAGE_SIZE should round down to the appropriate page
        for (unsigned i = location / PAGE_SIZE; i < table_size; i++) {
                frame_table[i].used = true;
                frame_table[i].next_free = NULL;
        }

        // mark memory used so far by kernel as used
        // we need to round up
        unsigned highest_used = (ram_getfirstfree() + (PAGE_SIZE - 1)) / PAGE_SIZE;
        for (unsigned i = 0; i < highest_used; i++) {
                frame_table[i].used = true;
                frame_table[i].next_free = NULL;
        }
        next_free = &(frame_table[highest_used]);
        frame_table[highest_used - 1].next_free = next_free;

        // mark everything else as free memory
        for (unsigned i = highest_used; i < location / PAGE_SIZE; i++) {
                frame_table[i].used = false;
                frame_table[i].next_free = &(frame_table[i + 1]);
        }
}

/* Note that this function returns a VIRTUAL address, not a physical 
 * address
 * WARNING: this function gets called very early, before
 * vm_bootstrap().  You may wish to modify main.c to call your
 * frame table initialisation function, or check to see if the
 * frame table has been initialised and call ram_stealmem() otherwise.
 */

vaddr_t alloc_kpages(unsigned int npages)
{
        if (frame_table == NULL) {
                paddr_t addr;

                spinlock_acquire(&stealmem_lock);
                addr = ram_stealmem(npages);
                spinlock_release(&stealmem_lock);

                if(addr == 0)
                        return 0;

                return PADDR_TO_KVADDR(addr);
        }
        else {
                // TODO: this is for debugging only - remove later
                KASSERT(npages == 1);

                if (npages != 1) {
                        return (vaddr_t) NULL;
                }
                return (vaddr_t) NULL;
        }
}

void free_kpages(vaddr_t addr)
{
        (void) addr;
}

