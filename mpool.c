
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mpool.h"

/* The basic data structure describing a free space arena element */
typedef struct block {
    int size;                  /**< Size of the data payload */
    struct block *prev, *next; /**< Pointer to the previous/next block */
} block_t;

/* Size of a memory element, 32 or 64 bits */
enum {
    word_size = __SIZE_WIDTH__ / 8, /**< size of memory element */
#if __SIZE_WIDTH__ == 64
    log2_word_size = 3,
#else
    log2_word_size = 2,
#endif
    header_size = 3 * word_size, /**< size, previous/next block addresses */
};

/* Current free space manipulated by the arena */
static block_t *current;

/* temporary struct used when forking/merging blocks */
static block_t *tmp_block;

/* Used to store original block address before parsing the free space blocks */
static void *tmp_pt;

/* Used to track arena status during usage and check if no leaks occcur */
static int pool_size;
static int pool_free_space;

/* Find free space when allocating */
static inline void *get_loc_to_place(void *addr, int place);

bool pool_init(void *addr, int size)
{
    if (!addr) /* not a valid memory address */
        return false;

    if (size <= header_size) /* size is too small, can notstore a header */
        return false;

    tmp_block = 0;
    tmp_pt = 0;

    pool_size = size - word_size;
    pool_free_space = size - word_size;

    current = (block_t *) addr;
    current->size = pool_free_space;
    current->prev = NULL, current->next = NULL;

    return true;
}

/* Round up a size to the next multiple of 32 or 64 bits size */
static inline int round_up(const int *x)
{
    return ((*x + 7) >> log2_word_size) << log2_word_size;
}

void *pool_malloc(int size)
{
    if (!size)
        return NULL;

    /* Round up the size up to the arch width. Ensure the size is at minimum a
     * register size and a multiple of that register. So if use 64 bits arch, a
     * 4 bytes allocation is round up to 8 bytes, and 28 bytes is round up to
     * 32 bytes.
     */
    int _size = round_up(&size);

    /* Return NULL if failed to allocate a buffer */
    if (pool_free_space <= (_size + header_size))
        return NULL;

    void *ret = get_loc_to_place(current, _size);

    if (!ret)
        return NULL;

    /* payload's address the application can use */
    ret = (char *) current + word_size;

    /* Store current's info before moving its head */
    void *next_pt = current->next, *prev_pt = current->prev;
    int new_size = current->size - word_size - _size;

    /* Adjust free space of current block and update its meta-data */
    void *new_space = (char *) current + word_size + _size;
    current = new_space;

    if (new_size < 0) {
        current->size = 0;
        return NULL;
    }
    current->size = new_size;

    current->prev = prev_pt, current->next = next_pt;

    /* Update previous block to link current */
    if (current->prev) {
        tmp_block = prev_pt;
        tmp_block->next = current;
    }
    /* Update next block to link current, only if exists */
    if (current->next) {
        tmp_block = next_pt;
        tmp_block->prev = current;
    }

    return ret;
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
static inline void *get_loc_to_place(void *current, int size)
{
    block_t *parse = current;
    if (parse->size >= (size + header_size))
        return current;

    /* parse the prev blocks to find a place */
    for (parse = ((block_t *) current)->prev; parse; parse = parse->prev) {
        if (parse->size >= (size + header_size))
            return parse;
    }

    /* parse the next blocks to find a place */
    for (parse = ((block_t *) current)->prev; parse; parse = parse->next) {
        if (parse->size >= (size + header_size))
            return parse;
    }

    /* No space found, stop the allocation */
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
static void *get_loc_to_free(void *addr)
{
    /* In case the free block is monolithic, just return its address */
    if (!current->prev && !current->next)
        return &current;

    /* The current block of free space manipulated */
    tmp_pt = (block_t *) &current;
    tmp_block = tmp_pt;

    /* Location found to place the bloc under release */
    void *loc = NULL;

    /* The list is ordered by address, so we can divide the parsing to select
     * directly the right direction.
     */
    if ((uintptr_t) addr < (uintptr_t) tmp_pt) {
        for (;; tmp_block = tmp_block->prev) {
            loc = (block_t *) &tmp_block;
            /* No more free space on smaller address range, so when can place
             * this block on left of the current tmp / current free space.
             */
            if (!tmp_block->prev)
                break;

            /* Next free block has a smaller address, so we are place between
             * two blocks: free.prev < data block < tmp / currrent free space.
             */
            if ((uintptr_t) addr > (uintptr_t) tmp_block->prev)
                break;
        }
    } else {
        for (;; tmp_block = tmp_block->next) {
            loc = (block_t *) &tmp_block;
            /* No more free space on higher address range, so when can place
             * this block on right of the current tmp / current free space.
             */
            if (!tmp_block->next)
                break;

            /* Next free block has a higher address, so we are place between
             * two blocks: free.prev < data block < tmp / currrent free space.
             */
            if ((uintptr_t) addr < (uintptr_t) tmp_block->next)
                break;
        }
    }

    return loc;
}

void pool_free(void *addr)
{
    /* Get block info */
    void *block_pt = (char *) addr - word_size;
    block_t *block = block_pt;

    /* Update pool arena statistics */
    pool_free_space += block->size;

    /* Region is used to check if the block to release is adjacent to a free
     * space.
     */
    void *region;

    /* Free space zone to connect or merge with the block to release. Multiple
     * free blocks are suitable to connect, ensuring we will parse fastly the
     * linked list.
     */
    void *free_pt = get_loc_to_free(block_pt);
    block_t *free_block = (block_t *) free_pt;

    /* Check the block can be merged with the current free space selected */
    if ((uintptr_t) block_pt < (uintptr_t) free_pt) {
        /* If block space matches free space start address, merge */
        region = (char *) block_pt + block->size + word_size;
        if (region == free_pt) {
            block->prev = free_block->prev, block->next = free_block->next;
            block->size += free_block->size + header_size;
            pool_free_space += word_size;
            /* If a next block exists, connect it */
            if (block->next) {
                tmp_block = block->next;
                tmp_block->prev = block_pt;
            }
            /* If a previsou block exists, connect it */
            if (block->prev) {
                tmp_block = block->prev;
                tmp_block->next = block_pt;
            }
        }
    } else {
        /* if free space range macthes the block start address, merge in this
         * case, the free space becomes the block to release
         */
        region = (char *) free_pt + free_block->size + header_size;
        if (region == block_pt) {
            free_block->size += block->size + word_size;
            block_pt = free_pt;
            block = block_pt;
            pool_free_space += word_size;
        }
    }

    /* Try to merge with next block if exists */
    if (block->next) {
        /* if next block is contiguous the one to free, merge them */
        region = (char *) block_pt + block->size + word_size;
        if (region == block->next) {
            /* extend block size with next size */
            tmp_block = block->next;
            block->size += tmp_block->size + word_size;
            /* link next->next block with current block */
            tmp_block = tmp_block->next;
            tmp_block->prev = block_pt;
            /* Update pool's statistics */
            pool_free_space += word_size;
        }
    }

    /* Try to merge with previous block if exists */
    if (block->prev) {
        /* if previous block is contiguous the one to free, merge them */
        tmp_block = block->prev;
        region = (char *) block->prev + tmp_block->size + header_size;
        if (region == block_pt) {
            /* Update previous block by extending its size with block */
            tmp_block->size += word_size + block->size;
            /* Link block - 1 and block + 1 together */
            tmp_block->next = block->next;
            /* Current block's prev becomes the new current block */
            block = block->prev;
            /* (block + 1)'s prev is now linked to orignal (block - 1) */
            tmp_block = block->next;
            tmp_block->prev = block;
            /* Update pool's statistics */
            pool_free_space += word_size;
        }
    }
}