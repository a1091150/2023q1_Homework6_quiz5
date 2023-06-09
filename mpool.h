#pragma once

#include <stdbool.h>

/*
 * A basic malloc/calloc/free implementation.
 * The memory pool is divided into chunks, each storing meta-data to store its
 * size and the links to the previous and next free blocks.
 *
 * Blocks can be allocated with malloc() (or calloc()=malloc()+erase()). Blocks
 * are released with free(), merging the block with the previous/next ones if
 * also available (can be erase() on free()).
 *
 * ┌────────┬─────────┬────────────┬────────┬──────┬─────────┬──────┐
 * │Block 0 │ Block 1 │ Free Space │ Block 3│ Free │ Block 4 │ ...  │
 * └────────┴─────────┴────────────┴────────┴──────┴─────────┴──────┘
 *
 * malloc()/free() combo only manipulates the free space, forking it to
 * allocate a chunk, merging it to release a chunk.
 *
 * # Algorithm
 *
 * An allocated block is composed first by a register of 32 or 64 bits wide
 * based on the CPU architecture, describing the size in bytes, then by the
 * payload.
 *
 * A free block is composed first by a register describing the block size,
 * then by two pointers. So the free space is described by a linked list to
 * parse and manipulate it fastly.
 *
 *
 *                            Free block                  In-use block
 *
 *                        ┌────────────────┐           ┌────────────────┐
 *                        │      Size      │           │      Size      │
 *                        ├────────────────┤           ├────────────────┤
 *                        │ Next Block Ptr │           │                │
 *                        ├────────────────┤           │                │
 *                        │Prev. Block Ptr │           │    Payload     │
 *                        ├────────────────┤           │                │
 *                        │                │           │                │
 *                        │    ........    │           │                │
 *                        │                │           │                │
 *                        └────────────────┘           └────────────────┘
 *
 * Free blocks store the links to the next and previous free block, so,
 * malloc() and free() can use these links to parse the pool, allocate a chunk
 * or release it. On allocation, if no region is able to store the requested
 * payload size, returns -1.
 *
 * # malloc()
 *
 * Parse the free space to find a chunk. Check if current block can contain the
 * requested payload:
 *
 * 1. If yes, fork the block:
 *   - If not consume the whole space, set the first word as the register as the
 *     size allocated. The new allocated block is placed on the tail of the free
 *     space. Adjust the size of the free block.
 *   - If yes, erase the prv/nxt pointers and return the payload address
 *   - Erase payload if use secure alloc()
 *   - Return pyaload address
 * 2. If no, parse the linked list
 *    - Store the original address
 *    - Move thru the previous block and check if some can be used
 *    - If no block is big enough, move to the original address then parse the
 *      next blocks
 *    - If a block is found, apply (1)
 *    - If not, return -1
 *
 * # free()
 *
 * 1. Locate the block to free accross the chained list. nxt/prv pointers are
 *    used to move until the position is found. Ensure the block is not the
 *    last or the first around the space boundary
 * 2. If next block is contiguous, merge it:
 *   - Erase next block info if existing
 *   - Add current block size with next block size
 *   - Update next.nxt block to point to the new current block address
 * 3. If previous block is contiguous, merge it:
 *   - update the size of the previous block by adding the chunk size
 *   - update the current.nxt block's prv pointer to the new merged block
 * address
 */

/* Called by the environment to setup the arena start address.
 * To call once when the system boots up or when creating
 * a new pool arena.
 *   @addr: address of the arena's first byte
 *   @size: size in byte available for the arena
 * Returns:
 *   false if size is too small to contain at least 1 byte, otherwise true
 */
bool pool_init(void *addr, int size);

/* Memory allocation.
 * Allocates in the arena a buffer of @size bytes. Memory blocked reserved in
 * memory are always boundary aligned with the hardware architecture, so
 * 4 bytes for 32 bits architecture, or 8 bytes for 64 bits architecture. Use
 * first-fit startegy for the moment.
 *   @size: the number of bytes the block needs to own
 * Returns:
 *   the address of the buffer's first byte, otherwise -1 if failed
 */
void *pool_malloc(int size);

/* Clear allocation.
 * Same as pool_malloc() but erase with zero the zone allocated.
 *   @size: the number of bytes the block needs to own
 * Returns:
 *  - the address of the buffer's first byte, otherwise -1 if failed
 */
void *pool_calloc(int size);

/* Used to place existibng block in a wider space.
 * If failed, the existing block remains.
 *   @addr: address of the chunk to move
 *   @size: size in byte of the chunk
 * Returns:
 *   NULL if failed to allocate the new block
 */
void *pool_realloc(void *addr, int size);

/* Releases a block and make it available again for future use.
 *   @addr: the address of the data block
 */
void pool_free(void *addr);