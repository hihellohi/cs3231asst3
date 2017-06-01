/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *        The President and Fellows of Harvard College.
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

struct addrspace *
as_create(void)
{
        struct addrspace *as;

        as = kmalloc(sizeof(struct addrspace));
        if (as == NULL) {
                return NULL;
        }

        as->first_region = NULL;
        as->heap = NULL;
        as->writeable_mask = 0;

        return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
        struct addrspace *newas;

        newas = as_create();
        if (newas==NULL) {
                return ENOMEM;
        }

        as_region *curold, *curnew;
        for(curold = &old->first_region, curnew = &newas->first_region; 
                        *curold;
                        curold = &((*curold)->next), curnew = &((*curnew)->next)){

                *curnew = kmalloc(sizeof(struct _as_region));
                if(!*curnew){
                        as_destroy(newas);
                        return ENOMEM;
                }

                **curnew = **curold;
        }

        int err = vm_copy(old, newas);
        if(err) {
                as_destroy(newas);
                return err;
        }

        *ret = newas;
        return 0;
}

void
as_destroy(struct addrspace *as)
{
        vm_destroy(as);
        as_region cur = as->first_region, old;
        while(cur){
                old = cur;
                cur = cur->next;
                kfree(old);
        }

        kfree(as);
}

void
as_activate(void)
{
        struct addrspace *as;

        as = proc_getas();
        if (as == NULL) {
                /*
                 * Kernel thread without an address space; leave the
                 * prior address space in place.
                 */
                return;
        }

        // copied from dumbvm.c
        int i, spl;
        /* Disable interrupts on this CPU while frobbing the TLB. */
        spl = splhigh();

        for (i=0; i<NUM_TLB; i++) {
                tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
        }

        splx(spl);
}

void
as_deactivate(void)
{
        /*
         * Write this. For many designs it won't need to actually do
         * anything. See proc.c for an explanation of why it (might)
         * be needed.
         */
        as_activate();
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
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
                 int readable, int writeable, int executable)
{
        as_region new = kmalloc(sizeof(struct _as_region));
        if(!new){
                return ENOMEM;
        }

        if(!(readable || writeable || executable)) {
                kfree(new);
                return 0;
        }

        new->next = as->first_region;
        as->first_region = new;

        new->vbase = vaddr;
        new->size = memsize;
        new->writeable = writeable;

        return 0;
}

int
as_prepare_load(struct addrspace *as)
{
        as->writeable_mask = TLBLO_DIRTY;
        as_activate();
        return 0;
}

int
as_complete_load(struct addrspace *as)
{
        as->writeable_mask = 0;
        as_activate();
        return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
        /* Initial user-level stack pointer */
        *stackptr = USERSTACK;
        as->stack_pointer = USERSTACK - (PAGE_SIZE * 16);
        int err = as_define_region(as, as->stack_pointer, PAGE_SIZE * 16, 1, 1, 0);
        if(err) {
                as->stack_pointer = 0;
                return err;
        }

        return 0;
}

