#include <driver/vga.h>
#include <zjunix/bootmm.h>
#include <zjunix/buddy.h>
#include <zjunix/list.h>
#include <zjunix/lock.h>
#include <zjunix/utils.h>

#define Allign(x, y) (((x)+((y)-1)) & ~((y)-1))

unsigned int kernel_start_pfn, kernel_end_pfn;

struct page *pages;
struct buddy_sys buddy;

// void set_bplevel(struct page* bp, unsigned int bplevel)
//{
//	bp->bplevel = bplevel;
//}

void buddy_info() {
    unsigned int index;
    kernel_printf("Buddy-system :\n");
    kernel_printf("\tstart page-frame number : %x\n", buddy.buddy_start_pfn);
    kernel_printf("\tend page-frame number : %x\n", buddy.buddy_end_pfn);
    for (index = 0; index <= MAX_BUDDY_ORDER; ++index) {
        kernel_printf("\t(%x)# : %x frees\n", index, buddy.freelist[index].nr_free);
    }
}

// init all memory with page struct
void init_pages(unsigned int start_pfn, unsigned int end_pfn) {
    unsigned int i;
    for (i = start_pfn; i < end_pfn; i++) {
        clean_flag(pages + i, -1);
        set_flag(pages + i, _PAGE_RESERVED);
        (pages + i)->reference = 1;
        (pages + i)->virtual = (void *)(-1);
        (pages + i)->bplevel = (-1);
        (pages + i)->slabp = 0;  // initially, the free space is the whole page
        INIT_LIST_HEAD(&(pages[i].list));
    }
}

void init_buddy() {
    unsigned int bpsize = sizeof(struct page);
    unsigned char *bp_base;
    unsigned int i;

    bp_base = bootmm_alloc_pages(bpsize * bmm.max_pfn, _MM_KERNEL, 1 << PAGE_SHIFT);
    if (!bp_base) {
        // the remaining memory must be large enough to allocate the whole group
        // of buddy page struct
        kernel_printf("\nERROR : bootmm_alloc_pages failed!\nInit buddy system failed!\n");
        while (1)
            ;
    }
    pages = (struct page *)((unsigned int)bp_base | 0x80000000);

    init_pages(0, bmm.max_pfn);

    kernel_start_pfn = 0;
    kernel_end_pfn = 0;
    for (i = 0; i < bmm.cnt_infos; ++i) {
        if (bmm.info[i].end > kernel_end_pfn)
            kernel_end_pfn = bmm.info[i].end;
    }
    kernel_end_pfn >>= PAGE_SHIFT;

    buddy.buddy_start_pfn = Allign(kernel_end_pfn,1<<MAX_BUDDY_ORDER);          // the pages that bootmm using cannot be merged into buddy_sys
    buddy.buddy_end_pfn = bmm.max_pfn & ~((1 << MAX_BUDDY_ORDER) - 1);  // remain 2 pages for I/O

    // init freelists of all bplevels
    for (i = 0; i < MAX_BUDDY_ORDER + 1; i++) {
        buddy.freelist[i].nr_free = 0;
        INIT_LIST_HEAD(&(buddy.freelist[i].free_head));
    }
    buddy.start_page = pages + buddy.buddy_start_pfn;
    init_lock(&(buddy.lock));

    for (i = buddy.buddy_start_pfn; i < buddy.buddy_end_pfn; ++i) {
        __free_pages(pages + i, 0);
    }
}

void __free_pages(struct page *pbpage, unsigned int bplevel) {
    /* page_idx -> the current page
     * bgroup_idx -> the buddy group that current page is in
     */
    unsigned int page_idx, bgroup_idx;
    unsigned int combined_idx, tmp;
    struct page *bgroup_page;

    lockup(&buddy.lock);

    page_idx = pbpage - buddy.start_page;
    // complier do the sizeof(struct) operation, and now page_idx is the index

    while (bplevel < MAX_BUDDY_ORDER) {
        bgroup_idx = page_idx ^ (1 << bplevel);
        bgroup_page = pbpage + (bgroup_idx - page_idx);
        #ifdef budd_debug
        kernel_printf("group%x %x\n", (page_idx), bgroup_idx);
        #endif
        if(bgroup_page->flag!=0)//the page has been allocated
            break;

        if (!_is_same_bplevel(bgroup_page, bplevel)) {
            #ifdef budd_debug
            kernel_printf("%x %x\n", bgroup_page->bplevel, bplevel);
            #endif
            break;
        }
        list_del_init(&bgroup_page->list);
        --buddy.freelist[bplevel].nr_free;
        set_bplevel(bgroup_page, -1);
        combined_idx = bgroup_idx & page_idx;
        pbpage =pbpage + (combined_idx - page_idx);
        page_idx = combined_idx;
        bplevel++;
    }

    set_bplevel(pbpage, bplevel);

//    (*(pbpage).flag) = 0;//buddy free

    list_add(&(pbpage->list), &(buddy.freelist[bplevel].free_head));
#ifdef budd_debug  
    kernel_printf("v%x__addto__%x\n", pbpage->list, buddy.freelist[bplevel].free_head);
#endif
     ++buddy.freelist[bplevel].nr_free;
    unlock(&buddy.lock);
}

struct page *__alloc_pages(unsigned int bplevel) {
    unsigned int current_order, size;
    struct page *page, *buddy_page;
    struct freelist *free;
    // kernel_printf("enter __alloc_pages\n");
    lockup(&buddy.lock);
    //search pages
    for (current_order = bplevel; current_order <= MAX_BUDDY_ORDER; ++current_order) {
        free = buddy.freelist + current_order;
        // kernel_printf("free == %x\n", free);
        if (!list_empty(&(free->free_head)))
            goto found;
    }
    //if not found
    unlock(&buddy.lock);
    // kernel_printf("__alloc_pages not found\n");
    return 0;

found:
    // kernel_printf("have found\n");
    page = container_of(free->free_head.next, struct page, list);
    list_del_init(&(page->list));
    set_bplevel(page, bplevel);
   // set_flag(page, _PAGE_ALLOCED);
   // (*(page)).flag = _PAGE_ALLOCED;
   set_flags(page, _PAGE_ALLOCED);
    // set_ref(page, 1);
    --(free->nr_free);

    size = 1 << current_order;
    while (current_order > bplevel) {
        --free;
        --current_order;
        size >>= 1;
        buddy_page = page + size;
        list_add(&(buddy_page->list), &(free->free_head));//add into free list 
        ++(free->nr_free);
        set_bplevel(buddy_page, current_order);
        //(*(buddy_page)).flag = 0;//set to 0
        set_flags(buddy_page, _PAGE_ALLOCED);
    }

    unlock(&buddy.lock);
    // kernel_printf("\n return page \n");
    return page;
}

void *alloc_pages(unsigned int level) {

    unsigned int bplevel = 0;
    if(level==0)
        return 0;

    while(1<<bplevel<level)
        bplevel++;

    // kernel_printf("bplevel == %x", bplevel);

    struct page *page = __alloc_pages(bplevel);

    if (!page)
        {
            // kernel_printf("page ==0\n");
            return 0;
        }

    // kernel_printf("return (void *)((page - pages) << PAGE_SHIFT) \n");
    return (void *)((page - pages) << PAGE_SHIFT);
}

void free_pages(void *addr, unsigned int bplevel) {
    __free_pages(pages + ((unsigned int)addr >> PAGE_SHIFT), bplevel);
}
