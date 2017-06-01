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
static struct frame_table_entry *next_free = NULL;
struct page_table_entry **page_table = NULL;

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

void frametable_bootstrap(void) {
        paddr_t top_of_ram = ram_getsize();
        size_t nframes =  top_of_ram / PAGE_SIZE;
        paddr_t location = top_of_ram - (nframes * sizeof(struct frame_table_entry));
        frame_table = (struct frame_table_entry *) PADDR_TO_KVADDR(location);

        table_size = nframes * 2;

        location -= table_size * sizeof(struct page_table_entry *);
        page_table = (struct page_table_entry **) PADDR_TO_KVADDR(location);

        // mark memory used by frame_table and page_table as used
        // location / PAGE_SIZE should round down to the appropriate page
        for (size_t i = location / PAGE_SIZE; i < nframes; i++) {
                frame_table[i].ref_count = 1;
                frame_table[i].next_free = NULL;
        }
        for (size_t i = 0; i < table_size; i++) {
                page_table[i] = NULL;
        }

        // mark memory used so far by kernel as used
        // we need to round up
        size_t highest_used = (ram_getfirstfree() + PAGE_SIZE - 1) / PAGE_SIZE;
        for (size_t i = 0; i < highest_used; i++) {
                frame_table[i].ref_count = 1;
                frame_table[i].next_free = NULL;
        }
        next_free = &(frame_table[highest_used]);

        // mark everything else as free memory
        for (size_t i = highest_used; i < location / PAGE_SIZE; i++) {
                frame_table[i].ref_count = 0;
                frame_table[i].next_free = &(frame_table[i + 1]);
        }
        frame_table[(location / PAGE_SIZE) - 1].next_free = NULL;
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
        paddr_t addr;

        if (frame_table == NULL) {
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
                        return 0;
                }

                spinlock_acquire(&stealmem_lock);
                if (next_free == NULL) {
                        addr = 0;
                }
                else {
                        addr = PADDR_TO_KVADDR((next_free - frame_table) * PAGE_SIZE);
                        next_free->ref_count = 1;
                        next_free = next_free->next_free;
                }
                spinlock_release(&stealmem_lock);

                return addr;
        }
}

void free_kpages(vaddr_t addr)
{
        if (frame_table == NULL) {
                return;
        }
        paddr_t paddr = KVADDR_TO_PADDR(addr);
        unsigned entry = paddr / PAGE_SIZE;

        spinlock_acquire(&stealmem_lock);
        frame_table[entry].ref_count = 0;

        frame_table[entry].next_free = next_free;
        next_free = &frame_table[entry];
        spinlock_release(&stealmem_lock);
}

