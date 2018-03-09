/* -*-  Mode:C; c-basic-offset:4; tab-width:4 -*-
 *
 * (C) 2016 - Juergen Gross, SUSE Linux GmbH
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <inttypes.h>
#include <errno.h>
#include <xen/xen.h>
#include <xen/memory.h>

#if (defined __X86_32__) || (defined __X86_64__)
#include <xen-x86/mm.h>
#elif (defined __ARM_32__) || (defined __ARM_64__)
#include <xen-arm/mm.h>
#endif

#include <uk/plat/balloon.h>
#include <uk/plat/lcpu.h>
#include <uk/print.h>

unsigned long nr_max_pages;
unsigned long nr_mem_pages;

void get_max_pages(void)
{
	long ret;
	domid_t domid = DOMID_SELF;

    ret = HYPERVISOR_memory_op(XENMEM_maximum_reservation, &domid);
    if ( ret < 0 )
    {
        uk_printk("Could not get maximum pfn\n");
        return;
    }

    nr_max_pages = ret;
    uk_printk("Maximum memory size: %ld pages\n", nr_max_pages);
    uk_printd(DLVL_ERR, "In real get_max_pages(): %ld\n", nr_max_pages);
}

void mm_alloc_bitmap_remap(void)
{

}

#define N_BALLOON_FRAMES 64
static unsigned long balloon_frames[N_BALLOON_FRAMES];

int balloon_up(unsigned long n_pages)
{

    return n_pages;
}

static int in_balloon;

int chk_free_pages(unsigned long needed)
{
    unsigned long n_pages;

    /* No need for ballooning if plenty of space available. */
    if ( needed + BALLOON_EMERGENCY_PAGES <= nr_free_pages )
        return 1;

    /* If we are already ballooning up just hope for the best. */
    if ( in_balloon )
        return 1;

    /* Interrupts disabled can't be handled right now. */
    if ( ukplat_lcpu_irqs_disabled() )
        return 1;

    in_balloon = 1;

    while ( needed + BALLOON_EMERGENCY_PAGES > nr_free_pages )
    {
        n_pages = needed + BALLOON_EMERGENCY_PAGES - nr_free_pages;
        if ( !balloon_up(n_pages) )
            break;
    }

    in_balloon = 0;

    return needed <= nr_free_pages;
}
