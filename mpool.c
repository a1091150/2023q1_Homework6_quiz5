
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "list.h"
#include "mpool.h"

/* The basic data structure describing a free space arena element */
typedef struct block {
    int size; /**< Size of the data payload */
    union {
        char *payload;
        struct {
            struct list_head list; /**< Pointer to the previous/next block */
        };
    };
} block_t;

/* Size of a memory element, 32 or 64 bits */
enum {
    word_size = sizeof(int), /**< size of memory element */
    log2_word_size = __builtin_ctz(sizeof(int)),
    header_size = sizeof(block_t), /**< size, previous/next block addresses */
};

static LIST_HEAD(block_head);

/* Used to track arena status during usage and check if no leaks occcur */
static int pool_size;
static int pool_free_space;

/* Find free space when allocating */
static inline block_t *get_loc_to_place(int place);
static inline struct list_head *get_loc_to_free(void *addr);
static inline void list_replace(struct list_head *from, struct list_head *to);
static inline void list_insert_before(struct list_head *node,
                                      struct list_head *after);
static inline void block_merge(struct list_head *node1,
                               struct list_head *node2);

bool pool_init(void *addr, int size)
{
    if (!addr) /* not a valid memory address */
        return false;

    if (size <= header_size) /* size is too small, can notstore a header */
        return false;

    pool_size = size - word_size;
    pool_free_space = size - word_size;

    block_t *current = (block_t *) addr;
    current->size = pool_free_space;
    list_add(&current->list, &block_head);
    return true;
}

/* Round up a size to the next multiple of 32 or 64 bits size (4 bytes or 8
 * bytes size) */
static inline int round_up(const int *x)
{
    const int offset = (1 << log2_word_size) - 1;
    return ((*x + offset) >> log2_word_size) << log2_word_size;
}

static inline void list_replace(struct list_head *from, struct list_head *to)
{
    *to = *from;
    to->next->prev = to;
    to->prev->next = to;
}

void *pool_malloc(int size)
{
    if (size <= 0)
        return NULL;

    int _size = round_up(&size);
    if (pool_free_space <= (_size + header_size))
        return NULL;

    block_t *ret = get_loc_to_place(size);

    if (!ret)
        return NULL;

    block_t *new_block = (block_t *) (&ret->payload + _size);
    new_block->size = ret->size - word_size - _size;
    ret->size = _size;
    list_replace(&ret->list, &new_block->list);
    pool_free_space -= _size;
    pool_free_space -= word_size;
    return &ret->payload;
}

void *pool_calloc(int size)
{
    void *ptr = pool_malloc(size);
    if (!ptr)
        return NULL;

    memset(ptr, 0, size);
    return ptr;
}

void *pool_realloc(void *addr, int size)
{
    void *ptr = pool_malloc(size);
    if (!ptr)
        return NULL;

    memcpy(ptr, addr, size);
    pool_free(addr);
    return ptr;
}

/* Search for a free space to place a new block */
static inline block_t *get_loc_to_place(int size)
{
    block_t *node;
    list_for_each_entry (node, &block_head, list) {
        if (node->size >= (size + header_size))
            return node;
    }
    return NULL;
}

/* Parses the free blocks to find the place to set the one under release.
 * Useful to update the linked list correctly and fast its parsing.
 *
 * Follows the different cases to handle:
 * ┌───────┬────────────────────────────────────────────┐
 * │Block 0│          ~~~~~~~~ Free ~~~~~~~~~           │
 * └───────┴────────────────────────────────────────────┘
 * ┌────────────────────────────────────────────┬───────┐
 * │          ~~~~~~~~ Free ~~~~~~~~~           │Block 0│
 * └────────────────────────────────────────────┴───────┘
 * ┌───────────────┬───────┬───────────────────────────┐
 * │ ~~~ Free ~~~  │Block 0│  ~~~~~~~~ Free ~~~~~~~~   │
 * └───────────────┴───────┴───────────────────────────┘
 * ┌───────┬────────────────────────────────────┬───────┐
 * │Block 0│      ~~~~~~~~ Free ~~~~~~~~~       │Block 1│
 * └───────┴────────────────────────────────────┴───────┘
 * ┌───────┬───────┬────────────────────┬───────┬────────┬───────┬───────┐
 * │Block 0│Block 1│   ~~~ Free ~~~     │Block 2│~ Free ~│Block 3│Block 4│
 * └───────┴───────┴────────────────────┴───────┴────────┴───────┴───────┘
 * ┌────────┬───────┬───────┬────────────────────┬───────┬────────┐
 * │~ Free ~│Block 0│Block 1│   ~~~ Free ~~~     │Block 2│~ Free ~│
 * └────────┴───────┴───────┴────────────────────┴───────┴────────┘
 *
 *   @addr: pointer to an address to release
 * Returns:
 *   a pointer to the location where to place the block to release. The place
 *   to use can be on the left or on the right of address passed. If no place
 *   found, returns NULL.
 */
static inline struct list_head *get_loc_to_free(void *addr)
{
    /* In case the free block is monolithic, just return its address */
    if (list_is_singular(&block_head))
        return block_head.prev;

    block_t *target = container_of(addr, block_t, payload);
    block_t *node = NULL;

    list_for_each_entry (node, &block_head, list) {
        if ((uintptr_t) target < (uintptr_t) node)
            break;
    }

    return &node->list;
}

static inline void list_insert_before(struct list_head *node,
                                      struct list_head *after)
{
    struct list_head *prev = after->prev;
    node->prev = prev;
    node->next = after;
    after->prev = node;
    prev->next = node;
}

static void block_try_merge(struct list_head *node1, struct list_head *node2)
{
    if (node1 == &block_head || node2 == &block_head)
        return;

    block_t *n1 = container_of(node1, block_t, list);
    block_t *n2 = container_of(node2, block_t, list);
    uintptr_t loc = (uintptr_t) (&n1->payload + n1->size);
    if (loc == (uintptr_t) n2) {
        list_del(node2);
        n1->size += word_size + n2->size;
        pool_free_space += word_size;
    }
}

void pool_free(void *addr)
{
    block_t *target = container_of(addr, block_t, payload);
    pool_free_space += target->size;
    struct list_head *target_after = get_loc_to_free(addr);
    list_insert_before(&target->list, target_after);
    block_try_merge(&target->list, target->list.next);
    block_try_merge(target->list.prev, &target->list);
}