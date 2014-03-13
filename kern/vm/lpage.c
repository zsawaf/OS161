/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2009
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
#include <spinlock.h>
#include <synch.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <vmprivate.h>
#include <machine/coremap.h>
#include <current.h>

/* 
 * lpage operations
 */

/* Stats counters */
static volatile uint32_t ct_zerofills;
static volatile uint32_t ct_minfaults;
static volatile uint32_t ct_majfaults;
static volatile uint32_t ct_discard_evictions;
static volatile uint32_t ct_write_evictions;
static struct spinlock stats_spinlock = SPINLOCK_INITIALIZER;


void
vm_printstats(void)
{
	uint32_t zf, mn, mj, de, we, te;

	spinlock_acquire(&stats_spinlock);
	zf = ct_zerofills;
	mn = ct_minfaults;
	mj = ct_majfaults;
	de = ct_discard_evictions;
	we = ct_write_evictions;
	spinlock_release(&stats_spinlock);

	te = de+we;

	kprintf("vm: %lu zerofills %lu minorfaults %lu majorfaults\n",
		(unsigned long) zf, (unsigned long) mn, (unsigned long) mj);
	kprintf("vm: %lu evictions (%lu discarding, %lu writes)\n",
		(unsigned long) te, (unsigned long) de, (unsigned long) we);
	vm_printmdstats();
}

/*
 * Create a logical page object.
 * Synchronization: none.
 */
struct lpage *
lpage_create(void)
{
	struct lpage *lp;

	lp = kmalloc(sizeof(struct lpage));
	if (lp==NULL) {
		return NULL;
	}

	lp->lp_swapaddr = INVALID_SWAPADDR;
	lp->lp_paddr = INVALID_PADDR;
	spinlock_init(&lp->lp_spinlock);

	return lp;
}

/*
 * lpage_destroy: deallocates a logical page. Releases any RAM or swap
 * pages involved.
 *
 * Synchronization: Someone might be in the process of evicting the
 * page if it's resident, so it might be pinned. So lock and pin
 * together.
 *
 * We assume that lpages are not shared between address spaces and
 * address spaces are not shared between threads.
 */
void 					
lpage_destroy(struct lpage *lp)
{
	paddr_t pa;

	KASSERT(lp != NULL);

	lpage_lock_and_pin(lp);

	pa = lp->lp_paddr & PAGE_FRAME;
	if (pa != INVALID_PADDR) {
		DEBUG(DB_VM, "lpage_destroy: freeing paddr 0x%x\n", pa);
		lp->lp_paddr = INVALID_PADDR;
		lpage_unlock(lp);
		coremap_free(pa, false /* iskern */);
		coremap_unpin(pa);
	}
	else {
		lpage_unlock(lp);
	}

	if (lp->lp_swapaddr != INVALID_SWAPADDR) {
		DEBUG(DB_VM, "lpage_destroy: freeing swap addr 0x%llx\n", 
		      lp->lp_swapaddr);
		swap_free(lp->lp_swapaddr);
	}

	spinlock_cleanup(&lp->lp_spinlock);
	kfree(lp);
}


/*
 * lpage_lock & lpage_unlock
 *
 * A logical page may be accessed by more than one thread: not only
 * the thread that owns it, but also the pager thread if such a thing
 * should exist, plus anyone else who might be swapping the page out.
 *
 * Therefore, it needs to be locked for usage. We use a spinlock; to
 * avoid ballooning memory usage, it might be more desirable to use a
 * bare spinlock_data_t.
 *
 * It is more or less incorrect to wait on this lock for any great
 * length of time.
 * 
 *      lpage_lock: acquires the lock on an lpage.
 *      lpage_unlock: releases the lock on an lpage.
 */
void
lpage_lock(struct lpage *lp) 
{
	spinlock_acquire(&lp->lp_spinlock);
}

void
lpage_unlock(struct lpage *lp)
{
	KASSERT(spinlock_do_i_hold(&lp->lp_spinlock));
	spinlock_release(&lp->lp_spinlock);
}

/*
 * lpage_lock_and_pin
 *
 * Lock the lpage and also pin the underlying physical page (if any)
 * in the coremap. This requires a silly retry dance, because we need
 * to pin first but also need the physical address from the lpage to
 * do that. If the physical address changes while we were pinning the
 * page, retry.
 *
 * Note that you can't in general hold another lpage lock when calling
 * this, because it acquires the coremap spinlock, and then perhaps
 * waits to pin the physical page. The eviction path holds the coremap
 * spinlock and holds a page pinned while locking the lpage; so if
 * someone's trying to swap the other page out you can deadlock.
 *
 * However, if you've got the other lpage locked *and* its physical
 * page pinned, that can't happen, so it's safe to lock and pin
 * multiple pages.
 */
void
lpage_lock_and_pin(struct lpage *lp)
{
	paddr_t pa, pinned;

	pinned = INVALID_PADDR;
	lpage_lock(lp);
	while (1) {
		pa = lp->lp_paddr & PAGE_FRAME;
		/*
		 * If the lpage matches what we have (including on the
		 * first pass with INVALID_PADDR) we're done.
		 */
		if (pa == pinned) {
			break;
		}
		/*
		 * Otherwise we need to unpin, which means unlock the
		 * lpage too.
		 */
		lpage_unlock(lp);
		if (pinned != INVALID_PADDR) {
			coremap_unpin(pinned);
		}
		/*
		 * If what we just got out of the lpage is *now*
		 * invalid, because the page was paged out on us,
		 * we're done. The page can't be paged in again behind
		 * our back, so assert it hasn't after regrabbing the
		 * lpage lock.
		 */
		if (pa == INVALID_PADDR) {
			lpage_lock(lp);
			KASSERT((lp->lp_paddr & PAGE_FRAME) == INVALID_PADDR);
			break;
		}
		/* Pin what we got and try again. */
		coremap_pin(pa);
		pinned = pa;
		lpage_lock(lp);
	}
}

/*
 * lpage_materialize: create a new lpage and allocate swap and RAM for it.
 * Do not do anything with the page contents though.
 *
 * Returns the lpage locked and the physical page pinned.
 */

static
int
lpage_materialize(struct lpage **lpret, paddr_t *paret)
{
	struct lpage *lp;
	paddr_t pa;
	off_t swa;

	lp = lpage_create();
	if (lp == NULL) {
		return ENOMEM;
	}

	swa = swap_alloc();
	if (swa == INVALID_SWAPADDR) {
		lpage_destroy(lp);
		return ENOSPC;
	}
	lp->lp_swapaddr = swa;

	pa = coremap_allocuser(lp);
	if (pa == INVALID_PADDR) {
		/* lpage_destroy will clean up the swap */
		lpage_destroy(lp);
		return ENOSPC;
	}

	lpage_lock(lp);

	lp->lp_paddr = pa | LPF_DIRTY;

	KASSERT(coremap_pageispinned(pa));

	*lpret = lp;
	*paret = pa;
	return 0;
}

/*
 * lpage_copy: create a new lpage and copy data from another lpage.
 *
 * The synchronization for this is kind of unpleasant. We do it like
 * this:
 *
 *      1. Create newlp.
 *      2. Materialize a page for newlp, so it's locked and pinned.
 *      3. Lock and pin oldlp.
 *      4. Extract the physical address and swap address.
 *      5. If oldlp wasn't present,
 *      5a.    Unlock oldlp.
 *      5b.    Page in.
 *      5c.    This pins the page in the coremap.
 *      5d.    Leave the page pinned and relock oldlp.
 *      5e.    Assert nobody else paged the page in.
 *      6. Copy.
 *      7. Unlock the lpages first, so we can enter the coremap.
 *      8. Unpin the physical pages.
 *      
 */
int
lpage_copy(struct lpage *oldlp, struct lpage **lpret)
{
	struct lpage *newlp;
	paddr_t newpa, oldpa;
	off_t swa;
	int result;

	result = lpage_materialize(&newlp, &newpa);
	if (result) {
		return result;
	}
	KASSERT(coremap_pageispinned(newpa));

	/* Pin the physical page and lock the lpage. */
	lpage_lock_and_pin(oldlp);
	oldpa = oldlp->lp_paddr & PAGE_FRAME;

	/*
	 * If there is no physical page, we allocate one, which pins
	 * it, and then (re)lock the lpage. Since we are single-
	 * threaded (if we weren't, we'd hold the address space lock
	 * to exclude sibling threads) nobody else should have paged
	 * the page in behind our back.
	 */
	if (oldpa == INVALID_PADDR) {
		/*
		 * XXX this is mostly copied from lpage_fault
		 */
		swa = oldlp->lp_swapaddr;
		lpage_unlock(oldlp);
		oldpa = coremap_allocuser(oldlp);
		if (oldpa == INVALID_PADDR) {
			coremap_unpin(newlp->lp_paddr & PAGE_FRAME);
			lpage_destroy(newlp);
			return ENOMEM;
		}
		KASSERT(coremap_pageispinned(oldpa));
		lock_acquire(global_paging_lock);
		swap_pagein(oldpa, swa);
		lpage_lock(oldlp);
		lock_release(global_paging_lock);
		/* Assert nobody else did the pagein. */
		KASSERT((oldlp->lp_paddr & PAGE_FRAME) == INVALID_PADDR);
		oldlp->lp_paddr = oldpa;
	}

	KASSERT(coremap_pageispinned(oldpa));

	coremap_copy_page(oldpa, newpa);

	KASSERT(LP_ISDIRTY(newlp));

	lpage_unlock(oldlp);
	lpage_unlock(newlp);

	coremap_unpin(newpa);
	coremap_unpin(oldpa);

	*lpret = newlp;
	return 0;
}

/*
 * lpage_zerofill: create a new lpage and arrange for it to be cleared
 * to all zeros. The current implementation causes the lpage to be
 * resident upon return, but this is not a guaranteed property, and
 * nothing prevents the page from being evicted before it is used by
 * the caller.
 *
 * Synchronization: coremap_allocuser returns the new physical page
 * "pinned" (locked) - we hold that lock while we update the page
 * contents and the necessary lpage fields. Unlock the lpage before
 * unpinning, so it's safe to take the coremap spinlock.
 */
int
lpage_zerofill(struct lpage **lpret)
{
	struct lpage *lp;
	paddr_t pa;
	int result;

	result = lpage_materialize(&lp, &pa);
	if (result) {
		return result;
	}
	KASSERT(spinlock_do_i_hold(&lp->lp_spinlock));
	KASSERT(coremap_pageispinned(pa));

	/* Don't actually need the lpage locked. */
	lpage_unlock(lp);

	coremap_zero_page(pa);

	KASSERT(coremap_pageispinned(pa));
	coremap_unpin(pa);

	spinlock_acquire(&stats_spinlock);
	ct_zerofills++;
	spinlock_release(&stats_spinlock);

	*lpret = lp;
	return 0;
}

/*
 * lpage_fault - handle a fault on a specific lpage. If the page is
 * not resident, get a physical page from coremap and swap it in.
 * 
 * You do not yet need to distinguish a readonly fault from a write
 * fault. When we implement sharing, there will be a difference.
 *
 * Synchronization: Lock the lpage while checking if it's in memory. 
 * If it's not, unlock the page while allocating space and loading the
 * page in. This only works because lpages are not currently sharable.
 * The page should be locked again as soon as it is loaded, but be 
 * careful of interactions with other locks while modifying the coremap.
 *
 * After it has been loaded, the page must be pinned so that it is not
 * evicted while changes are made to the TLB. It can be unpinned as soon
 * as the TLB is updated. 
 */
int
lpage_fault(struct lpage *lp, struct addrspace *as, int faulttype, vaddr_t va)
{
	KASSERT(lp != NULL); // kernel pages never get paged out, thus never fault

	lock_acquire(global_paging_lock);
	if ((lp->lp_paddr & PAGE_FRAME) != INVALID_PADDR) {
		lpage_lock_and_pin(lp);
	} else {
		lpage_lock(lp);
	}
	lock_release(global_paging_lock);

	KASSERT(lp->lp_swapaddr != INVALID_SWAPADDR);

	paddr_t pa = lp->lp_paddr;
	int writable; // 0 if page is read-only, 1 if page is writable

    /* case 1 - minor fault: the frame is still in memory */
	if ((pa & PAGE_FRAME) != INVALID_PADDR) {

		/* make sure it's a minor fault */
		KASSERT(pa != INVALID_PADDR);

		/* Setting the TLB entry's dirty bit */
		writable = (faulttype != VM_FAULT_READ);

		/* update stats */
		spinlock_acquire(&stats_spinlock);
		ct_minfaults++;
		DEBUG(DB_VM, "\nlpage_fault: minor faults = %d.", ct_minfaults);
		spinlock_release(&stats_spinlock);

	} else {
		/* case 2 - major fault: the frame was swapped out to disk */

		/* make sure it is a major fault */
		KASSERT(pa == INVALID_PADDR);

		/* allocate a new frame */
		lpage_unlock(lp); // must not hold lpage locks before entering coremap
		pa = coremap_allocuser(lp); // do evict if needed, also pin coremap
		if ((pa & PAGE_FRAME)== INVALID_PADDR) {
			DEBUG(DB_VM, "lpage_fault: ENOMEM: va=0x%x\n", va);
			return ENOMEM;
		}
		KASSERT(coremap_pageispinned(pa));

		/* retrieving the content from disk */
		lock_acquire(global_paging_lock); // because swap_pagein needs it
		swap_pagein((pa & PAGE_FRAME), lp->lp_swapaddr); // coremap is already pinned above
		lpage_lock(lp);
		lock_release(global_paging_lock);

		/* assert that nobody else did the pagein */
		KASSERT((lp->lp_paddr & PAGE_FRAME) == INVALID_PADDR);

		/* now update PTE with new PFN */
		lp->lp_paddr = pa ; // page is clean

		/* Setting the TLB entry's dirty bit */
		writable = 0; // this way we can detect the first write to a page

		/* update stats */
		spinlock_acquire(&stats_spinlock);
		ct_majfaults++;
		DEBUG(DB_VM, "\nlpage_fault: MAJOR faults = %d", ct_majfaults);
		spinlock_release(&stats_spinlock);
	}

	/* check preconditions before update TLB/PTE */
	KASSERT(coremap_pageispinned(lp->lp_paddr));
	KASSERT(spinlock_do_i_hold(&lp->lp_spinlock));

	/* PTE entry is dirty if the instruction is a write */
	if (writable) {
		LP_SET(lp, LPF_DIRTY);
	}

	/* Put the new TLB entry into the TLB */
	KASSERT(coremap_pageispinned(lp->lp_paddr)); // done in both cases of above IF clause
	mmu_map(as, va, lp->lp_paddr, writable); // update TLB and unpin coremap
	lpage_unlock(lp);

	return 0;
}

/*
 * lpage_evict: Evict an lpage from physical memory.
 *
 * Synchronization: lock the lpage while evicting it. We come here
 * from the coremap and should
 * have pinned the physical page. This is why we must not hold lpage
 * locks while entering the coremap code.
 */
void
lpage_evict(struct lpage *lp)
{
	KASSERT(lp != NULL);
	lpage_lock(lp);

	KASSERT(lp->lp_paddr != INVALID_PADDR);
	KASSERT(lp->lp_swapaddr != INVALID_SWAPADDR);

	/* if the page is dirty, swap_pageout */
	if (LP_ISDIRTY(lp)) {
        lpage_unlock(lp); // release lock before doing I/O

		KASSERT(lock_do_i_hold(global_paging_lock));
		KASSERT(coremap_pageispinned(lp->lp_paddr));

        swap_pageout((lp->lp_paddr & PAGE_FRAME), lp->lp_swapaddr);
        lpage_lock(lp);
        KASSERT((lp->lp_paddr & PAGE_FRAME) != INVALID_PADDR);

		/* update stats */
		spinlock_acquire(&stats_spinlock);
		ct_write_evictions++;
	    DEBUG (DB_VM, "lpage_evict: evicting Dirty page 0x%x\n",
	    		(lp->lp_paddr & PAGE_FRAME));
		spinlock_release(&stats_spinlock);

	} else {

		/* if page is clean, just update stats */
		spinlock_acquire(&stats_spinlock);
		ct_discard_evictions++;
	    DEBUG (DB_VM, "lpage_evict: evicting Clean page 0x%x\n",
	    		(lp->lp_paddr & PAGE_FRAME));
		spinlock_release(&stats_spinlock);
	}


	/* modify PTE to indicate that the page is no longer in memory. */
	lp->lp_paddr = INVALID_PADDR;

	lpage_unlock(lp);
}
