#include <pmm.h>
#include <list.h>
#include <string.h>
#include <default_pmm.h>
#include <stdio.h>  // 新增：用于 cprintf 函数

// 空闲内存块管理结构：链表头 + 空闲页总数
free_area_t free_area;
#define free_list (free_area.free_list)  // 空闲块链表头
#define nr_free (free_area.nr_free)      // 空闲页总数

// 1. 初始化空闲链表（无参，仅初始化链表和计数器）
static void best_fit_init(void) {
    list_init(&free_list);  // 初始化双向链表
    nr_free = 0;            // 初始空闲页为 0
}

// 2. 初始化物理页为空闲块（将base开始的n个页标记为空闲，加入链表）
static void best_fit_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);  // 确保要初始化的页数大于0
    struct Page *p = base;

    // 遍历每个页，初始化状态
    for (; p != base + n; p++) {
        assert(PageReserved(p));  // 确保初始时页为"内核保留"状态
        p->flags = 0;             // 清空所有标志（取消保留）
        set_page_ref(p, 0);       // 引用计数设为0（空闲页无引用）
        p->property = 0;          // 非空闲块头页，property设为0
    }

    // 标记空闲块头页：记录块大小 + 设为空闲属性
    base->property = n;
    SetPageProperty(base);
    nr_free += n;  // 空闲页总数增加

    // 按物理地址升序插入空闲链表（确保链表有序，方便后续分配/合并）
    if (list_empty(&free_list)) {
        // 链表为空，直接加入链表
        list_add(&free_list, &(base->page_link));
    } else {
        // 遍历链表，找到插入位置（地址从小到大）
        list_entry_t *le = &free_list;
        while ((le = list_next(le)) != &free_list) {
            struct Page *page = le2page(le, page_link);
            if (base < page) {
                // 插入到当前页前面
                list_add_before(le, &(base->page_link));
                break;
            } else if (list_next(le) == &free_list) {
                // 遍历到链表尾，插入到最后
                list_add(le, &(base->page_link));
            }
        }
    }
}

// 3. Best-Fit 分配算法（找"最小且≥需求n"的空闲块，返回块头页）
static struct Page *best_fit_alloc_pages(size_t n) {
    assert(n > 0);  // 确保分配页数大于0
    if (n > nr_free) {
        return NULL;  // 空闲页不足，返回空
    }

    struct Page *best_page = NULL;  // 记录"最佳匹配"的块头页
    size_t min_size = ~0UL;         // 初始为最大无符号数（找最小满足条件的块）
    list_entry_t *le = &free_list;  // 从链表头开始遍历

    // 遍历所有空闲块，筛选最佳匹配
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        if (p->property >= n && p->property < min_size) {
            // 找到更小的满足条件的块，更新记录
            min_size = p->property;
            best_page = p;
        }
    }

    // 若找到最佳块，拆分并更新链表
    if (best_page != NULL) {
        list_entry_t *prev_le = list_prev(&(best_page->page_link));
        list_del(&(best_page->page_link));  // 从空闲链表中删除该块

        // 若块大小大于需求，拆分剩余部分（剩余部分仍为空闲块）
        if (best_page->property > n) {
            struct Page *new_page = best_page + n;  // 新块的头页（当前块后n个页）
            new_page->property = best_page->property - n;  // 新块大小 = 原大小 - 分配大小
            SetPageProperty(new_page);  // 标记新块为空闲块头页
            list_add(prev_le, &(new_page->page_link));  // 剩余块插回链表
        }

        nr_free -= n;  // 空闲页总数减少
        ClearPageProperty(best_page);  // 取消当前块的"空闲"标记（已分配）
    }

    return best_page;  // 返回分配到的块头页
}

// 4. 释放页并合并相邻空闲块（减少内存碎片）
static void best_fit_free_pages(struct Page *base, size_t n) {
    assert(n > 0);  // 确保释放页数大于0
    struct Page *p = base;

    // 遍历每个要释放的页，初始化状态
    for (; p != base + n; p++) {
        // 确保释放的页不是"内核保留页"且不是"空闲块头页"
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;         // 清空标志
        set_page_ref(p, 0);   // 引用计数设为0
    }

    // 标记释放块为空闲：头页记录大小 + 设为空闲属性
    base->property = n;
    SetPageProperty(base);
    nr_free += n;  // 空闲页总数增加

    // 按物理地址升序插入空闲链表
    if (list_empty(&free_list)) {
        list_add(&free_list, &(base->page_link));
    } else {
        list_entry_t *le = &free_list;
        while ((le = list_next(le)) != &free_list) {
            struct Page *page = le2page(le, page_link);
            if (base < page) {
                list_add_before(le, &(base->page_link));
                break;
            } else if (list_next(le) == &free_list) {
                list_add(le, &(base->page_link));
            }
        }
    }

    // 合并【前面相邻】的空闲块（若连续）
    list_entry_t *le = list_prev(&(base->page_link));
    if (le != &free_list) {
        p = le2page(le, page_link);
        // 前块的末尾地址 == 当前块的起始地址 → 连续
        if (p + p->property == base) {
            p->property += base->property;  // 合并大小：前块大小 + 当前块大小
            ClearPageProperty(base);        // 取消当前块的"空闲头页"标记
            list_del(&(base->page_link));   // 从链表删除当前块
            base = p;                       // 合并后，头页更新为前块
        }
    }

    // 合并【后面相邻】的空闲块（若连续）
    le = list_next(&(base->page_link));
    if (le != &free_list) {
        p = le2page(le, page_link);
        // 当前块的末尾地址 == 后块的起始地址 → 连续
        if (base + base->property == p) {
            base->property += p->property;  // 合并大小：当前块大小 + 后块大小
            ClearPageProperty(p);           // 取消后块的"空闲头页"标记
            list_del(&(p->page_link));      // 从链表删除后块
        }
    }
}

// 5. 返回当前空闲页总数
static size_t best_fit_nr_free_pages(void) {
    return nr_free;
}

// 6. 验证Best-Fit算法正确性（修复宏参数和头文件问题）
static void best_fit_check(void) {
    // 测试1：分配1个页，验证非空且状态正确
    struct Page *p1 = alloc_pages(1);
    assert(p1 != NULL && !PageReserved(p1) && !PageProperty(p1));
    
    // 测试2：分配2个连续页，验证与p1不重叠
    struct Page *p2 = alloc_pages(2);
    assert(p2 != NULL && p2 != p1);
    
    // 测试3：释放页后，验证空闲页总数增加
    size_t free_before = nr_free_pages();
    free_pages(p1, 1);  // 释放1个页
    free_pages(p2, 2);  // 释放2个页
    assert(nr_free_pages() == free_before + 3);  // 释放3个页，空闲数+3
    
    // 测试4：重新分配3个页，验证能拿到空闲块
    struct Page *p3 = alloc_pages(3);
    assert(p3 != NULL);
    
    // 测试5：释放3个页，验证合并正常
    free_pages(p3, 3);
    assert(nr_free_pages() == free_before + 3);
    
    cprintf("best_fit_check() succeeded!\n");  // 打印测试成功信息
}

// 7. Best-Fit内存管理器接口（绑定上述函数）
const struct pmm_manager best_fit_pmm_manager = {
    .name = "best_fit_pmm_manager",          // 管理器名称
    .init = best_fit_init,                  // 初始化空闲链表
    .init_memmap = best_fit_init_memmap,    // 初始化空闲块
    .alloc_pages = best_fit_alloc_pages,    // 分配页
    .free_pages = best_fit_free_pages,      // 释放页
    .nr_free_pages = best_fit_nr_free_pages,// 查空闲页总数
    .check = best_fit_check                 // 验证算法正确性
};
