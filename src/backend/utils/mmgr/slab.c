/*-------------------------------------------------------------------------
 *
 * slab.c
 *	  SLAB allocator definitions.
 *
 * SLAB is a MemoryContext implementation designed for cases where large
 * numbers of equally-sized objects are allocated (and freed).
 *
 *
 * Portions Copyright (c) 2017-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/slab.c
 *
 *
 * NOTE:
 *	The constant allocation size allows significant simplification and various
 *	optimizations over more general purpose allocators. The blocks are carved
 *	into chunks of exactly the right size (plus alignment), not wasting any
 *	memory.
 *
 *	The information about free chunks is maintained both at the block level and
 *	global (context) level. This is possible as the chunk size (and thus also
 *	the number of chunks per block) is fixed.
 *
 *	On each block, free chunks are tracked in a simple linked list. Contents
 *	of free chunks is replaced with an index of the next free chunk, forming
 *	a very simple linked list. Each block also contains a counter of free
 *	chunks. Combined with the local block-level freelist, it makes it trivial
 *	to eventually free the whole block.
 *
 *	At the context level, we use 'freelist' to track blocks ordered by number
 *	of free chunks, starting with blocks having a single allocated chunk, and
 *	with completely full blocks on the tail.
 *
 *	This also allows various optimizations - for example when searching for
 *	free chunk, the allocator reuses space from the fullest blocks first, in
 *	the hope that some of the less full blocks will get completely empty (and
 *	returned back to the OS).
 *
 *	For each block, we maintain pointer to the first free chunk - this is quite
 *	cheap and allows us to skip all the preceding used chunks, eliminating
 *	a significant number of lookups in many common usage patterns. In the worst
 *	case this performs as if the pointer was not maintained.
 *
 *	We cache the freelist index for the blocks with the fewest free chunks
 *	(minFreeChunks), so that we don't have to search the freelist on every
 *	SlabAlloc() call, which is quite expensive.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/ilist.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_memorychunk.h"
#include "utils/memutils_internal.h"

/*
 * SlabContext is a specialized implementation of MemoryContext.
 */
typedef struct SlabContext
{
	MemoryContextData header;	/* Standard memory-context fields */
	/* Allocation parameters for this context: */
	Size		chunkSize;		/* chunk size */
	Size		fullChunkSize;	/* chunk size including header and alignment */
	Size		blockSize;		/* block size */
	Size		headerSize;		/* allocated size of context header */
	int			chunksPerBlock; /* number of chunks per block */
	int			minFreeChunks;	/* min number of free chunks in any block */
	int			nblocks;		/* number of blocks allocated */
#ifdef MEMORY_CONTEXT_CHECKING
	bool	   *freechunks;		/* bitmap of free chunks in a block */
#endif
	/* blocks with free space, grouped by number of free chunks: */
	dlist_head	freelist[FLEXIBLE_ARRAY_MEMBER];
} SlabContext;

/*
 * SlabBlock
 *		Structure of a single block in SLAB allocator.
 *
 * node: doubly-linked list of blocks in global freelist
 * nfree: number of free chunks in this block
 * firstFreeChunk: index of the first free chunk
 */
typedef struct SlabBlock
{
	dlist_node	node;			/* doubly-linked list */
	int			nfree;			/* number of free chunks */
	int			firstFreeChunk; /* index of the first free chunk in the block */
	SlabContext *slab;			/* owning context */
} SlabBlock;


#define Slab_CHUNKHDRSZ sizeof(MemoryChunk)
#define SlabPointerGetChunk(ptr)	\
	((MemoryChunk *)(((char *)(ptr)) - sizeof(MemoryChunk)))
#define SlabChunkGetPointer(chk)	\
	((void *)(((char *)(chk)) + sizeof(MemoryChunk)))
#define SlabBlockGetChunk(slab, block, idx) \
	((MemoryChunk *) ((char *) (block) + sizeof(SlabBlock)	\
					+ (idx * slab->fullChunkSize)))
#define SlabBlockStart(block)	\
	((char *) block + sizeof(SlabBlock))
#define SlabChunkIndex(slab, block, chunk)	\
	(((char *) chunk - SlabBlockStart(block)) / slab->fullChunkSize)

/*
 * SlabContextCreate
 *		Create a new Slab context.
 *
 * parent: parent context, or NULL if top-level context
 * name: name of context (must be statically allocated)
 * blockSize: allocation block size
 * chunkSize: allocation chunk size
 *
 * The MAXALIGN(chunkSize) may not exceed MEMORYCHUNK_MAX_VALUE
 */
MemoryContext
SlabContextCreate(MemoryContext parent,
				  const char *name,
				  Size blockSize,
				  Size chunkSize)
{
	int			chunksPerBlock;
	Size		fullChunkSize;
	Size		freelistSize;
	Size		headerSize;
	SlabContext *slab;
	int			i;

	/* ensure MemoryChunk's size is properly maxaligned */
	StaticAssertStmt(Slab_CHUNKHDRSZ == MAXALIGN(Slab_CHUNKHDRSZ),
					 "sizeof(MemoryChunk) is not maxaligned");
	Assert(MAXALIGN(chunkSize) <= MEMORYCHUNK_MAX_VALUE);

	/* Make sure the linked list node fits inside a freed chunk */
	if (chunkSize < sizeof(int))
		chunkSize = sizeof(int);

	/* chunk, including SLAB header (both addresses nicely aligned) */
	fullChunkSize = Slab_CHUNKHDRSZ + MAXALIGN(chunkSize);

	/* Make sure the block can store at least one chunk. */
	if (blockSize < fullChunkSize + sizeof(SlabBlock))
		elog(ERROR, "block size %zu for slab is too small for %zu chunks",
			 blockSize, chunkSize);

	/* Compute maximum number of chunks per block */
	chunksPerBlock = (blockSize - sizeof(SlabBlock)) / fullChunkSize;

	/* The freelist starts with 0, ends with chunksPerBlock. */
	freelistSize = sizeof(dlist_head) * (chunksPerBlock + 1);

	/*
	 * Allocate the context header.  Unlike aset.c, we never try to combine
	 * this with the first regular block; not worth the extra complication.
	 */

	/* Size of the memory context header */
	headerSize = offsetof(SlabContext, freelist) + freelistSize;

#ifdef MEMORY_CONTEXT_CHECKING

	/*
	 * With memory checking, we need to allocate extra space for the bitmap of
	 * free chunks. The bitmap is an array of bools, so we don't need to worry
	 * about alignment.
	 */
	headerSize += chunksPerBlock * sizeof(bool);
#endif

	slab = (SlabContext *) malloc(headerSize);
	if (slab == NULL)
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while creating memory context \"%s\".",
						   name)));
	}

	/*
	 * Avoid writing code that can fail between here and MemoryContextCreate;
	 * we'd leak the header if we ereport in this stretch.
	 */

	/* Fill in SlabContext-specific header fields */
	slab->chunkSize = chunkSize;
	slab->fullChunkSize = fullChunkSize;
	slab->blockSize = blockSize;
	slab->headerSize = headerSize;
	slab->chunksPerBlock = chunksPerBlock;
	slab->minFreeChunks = 0;
	slab->nblocks = 0;

	/* initialize the freelist slots */
	for (i = 0; i < (slab->chunksPerBlock + 1); i++)
		dlist_init(&slab->freelist[i]);

#ifdef MEMORY_CONTEXT_CHECKING
	/* set the freechunks pointer right after the freelists array */
	slab->freechunks
		= (bool *) slab + offsetof(SlabContext, freelist) + freelistSize;
#endif

	/* Finally, do the type-independent part of context creation */
	MemoryContextCreate((MemoryContext) slab,
						T_SlabContext,
						MCTX_SLAB_ID,
						parent,
						name);

	return (MemoryContext) slab;
}

/*
 * SlabReset
 *		Frees all memory which is allocated in the given set.
 *
 * The code simply frees all the blocks in the context - we don't keep any
 * keeper blocks or anything like that.
 */
void
SlabReset(MemoryContext context)
{
	int			i;
	SlabContext *slab = castNode(SlabContext, context);

	Assert(slab);

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	SlabCheck(context);
#endif

	/* walk over freelists and free the blocks */
	for (i = 0; i <= slab->chunksPerBlock; i++)
	{
		dlist_mutable_iter miter;

		dlist_foreach_modify(miter, &slab->freelist[i])
		{
			SlabBlock  *block = dlist_container(SlabBlock, node, miter.cur);

			dlist_delete(miter.cur);

#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(block, slab->blockSize);
#endif
			free(block);
			slab->nblocks--;
			context->mem_allocated -= slab->blockSize;
		}
	}

	slab->minFreeChunks = 0;

	Assert(slab->nblocks == 0);
	Assert(context->mem_allocated == 0);
}

/*
 * SlabDelete
 *		Free all memory which is allocated in the given context.
 */
void
SlabDelete(MemoryContext context)
{
	/* Reset to release all the SlabBlocks */
	SlabReset(context);
	/* And free the context header */
	free(context);
}

/*
 * SlabAlloc
 *		Returns pointer to allocated memory of given size or NULL if
 *		request could not be completed; memory is added to the slab.
 */
void *
SlabAlloc(MemoryContext context, Size size)
{
	SlabContext *slab = castNode(SlabContext, context);
	SlabBlock  *block;
	MemoryChunk *chunk;
	int			idx;

	Assert(slab);

	Assert((slab->minFreeChunks >= 0) &&
		   (slab->minFreeChunks < slab->chunksPerBlock));

	/* make sure we only allow correct request size */
	if (size != slab->chunkSize)
		elog(ERROR, "unexpected alloc chunk size %zu (expected %zu)",
			 size, slab->chunkSize);

	/*
	 * If there are no free chunks in any existing block, create a new block
	 * and put it to the last freelist bucket.
	 *
	 * slab->minFreeChunks == 0 means there are no blocks with free chunks,
	 * thanks to how minFreeChunks is updated at the end of SlabAlloc().
	 */
	if (slab->minFreeChunks == 0)
	{
		block = (SlabBlock *) malloc(slab->blockSize);

		if (block == NULL)
			return NULL;

		block->nfree = slab->chunksPerBlock;
		block->firstFreeChunk = 0;
		block->slab = slab;

		/*
		 * Put all the chunks on a freelist. Walk the chunks and point each
		 * one to the next one.
		 */
		for (idx = 0; idx < slab->chunksPerBlock; idx++)
		{
			chunk = SlabBlockGetChunk(slab, block, idx);
			*(int32 *) MemoryChunkGetPointer(chunk) = (idx + 1);
		}

		/*
		 * And add it to the last freelist with all chunks empty.
		 *
		 * We know there are no blocks in the freelist, otherwise we wouldn't
		 * need a new block.
		 */
		Assert(dlist_is_empty(&slab->freelist[slab->chunksPerBlock]));

		dlist_push_head(&slab->freelist[slab->chunksPerBlock], &block->node);

		slab->minFreeChunks = slab->chunksPerBlock;
		slab->nblocks += 1;
		context->mem_allocated += slab->blockSize;
	}

	/* grab the block from the freelist (even the new block is there) */
	block = dlist_head_element(SlabBlock, node,
							   &slab->freelist[slab->minFreeChunks]);

	/* make sure we actually got a valid block, with matching nfree */
	Assert(block != NULL);
	Assert(slab->minFreeChunks == block->nfree);
	Assert(block->nfree > 0);

	/* we know index of the first free chunk in the block */
	idx = block->firstFreeChunk;

	/* make sure the chunk index is valid, and that it's marked as empty */
	Assert((idx >= 0) && (idx < slab->chunksPerBlock));

	/* compute the chunk location block start (after the block header) */
	chunk = SlabBlockGetChunk(slab, block, idx);

	/*
	 * Update the block nfree count, and also the minFreeChunks as we've
	 * decreased nfree for a block with the minimum number of free chunks
	 * (because that's how we chose the block).
	 */
	block->nfree--;
	slab->minFreeChunks = block->nfree;

	/*
	 * Remove the chunk from the freelist head. The index of the next free
	 * chunk is stored in the chunk itself.
	 */
	VALGRIND_MAKE_MEM_DEFINED(MemoryChunkGetPointer(chunk), sizeof(int32));
	block->firstFreeChunk = *(int32 *) MemoryChunkGetPointer(chunk);

	Assert(block->firstFreeChunk >= 0);
	Assert(block->firstFreeChunk <= slab->chunksPerBlock);

	Assert((block->nfree != 0 &&
			block->firstFreeChunk < slab->chunksPerBlock) ||
		   (block->nfree == 0 &&
			block->firstFreeChunk == slab->chunksPerBlock));

	/* move the whole block to the right place in the freelist */
	dlist_delete(&block->node);
	dlist_push_head(&slab->freelist[block->nfree], &block->node);

	/*
	 * And finally update minFreeChunks, i.e. the index to the block with the
	 * lowest number of free chunks. We only need to do that when the block
	 * got full (otherwise we know the current block is the right one). We'll
	 * simply walk the freelist until we find a non-empty entry.
	 */
	if (slab->minFreeChunks == 0)
	{
		for (idx = 1; idx <= slab->chunksPerBlock; idx++)
		{
			if (dlist_is_empty(&slab->freelist[idx]))
				continue;

			/* found a non-empty freelist */
			slab->minFreeChunks = idx;
			break;
		}
	}

	if (slab->minFreeChunks == slab->chunksPerBlock)
		slab->minFreeChunks = 0;

	/* Prepare to initialize the chunk header. */
	VALGRIND_MAKE_MEM_UNDEFINED(chunk, Slab_CHUNKHDRSZ);

	MemoryChunkSetHdrMask(chunk, block, MAXALIGN(slab->chunkSize),
						  MCTX_SLAB_ID);
#ifdef MEMORY_CONTEXT_CHECKING
	/* slab mark to catch clobber of "unused" space */
	if (slab->chunkSize < (slab->fullChunkSize - Slab_CHUNKHDRSZ))
	{
		set_sentinel(MemoryChunkGetPointer(chunk), size);
		VALGRIND_MAKE_MEM_NOACCESS(((char *) chunk) +
								   Slab_CHUNKHDRSZ + slab->chunkSize,
								   slab->fullChunkSize -
								   (slab->chunkSize + Slab_CHUNKHDRSZ));
	}
#endif

#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* fill the allocated space with junk */
	randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

	Assert(slab->nblocks * slab->blockSize == context->mem_allocated);

	return MemoryChunkGetPointer(chunk);
}

/*
 * SlabFree
 *		Frees allocated memory; memory is removed from the slab.
 */
void
SlabFree(void *pointer)
{
	int			idx;
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block = MemoryChunkGetBlock(chunk);
	SlabContext *slab = block->slab;

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	if (slab->chunkSize < (slab->fullChunkSize - Slab_CHUNKHDRSZ))
		if (!sentinel_ok(pointer, slab->chunkSize))
			elog(WARNING, "detected write past chunk end in %s %p",
				 slab->header.name, chunk);
#endif

	/* compute index of the chunk with respect to block start */
	idx = SlabChunkIndex(slab, block, chunk);

	/* add chunk to freelist, and update block nfree count */
	*(int32 *) pointer = block->firstFreeChunk;
	block->firstFreeChunk = idx;
	block->nfree++;

	Assert(block->nfree > 0);
	Assert(block->nfree <= slab->chunksPerBlock);

#ifdef CLOBBER_FREED_MEMORY
	/* XXX don't wipe the int32 index, used for block-level freelist */
	wipe_mem((char *) pointer + sizeof(int32),
			 slab->chunkSize - sizeof(int32));
#endif

	/* remove the block from a freelist */
	dlist_delete(&block->node);

	/*
	 * See if we need to update the minFreeChunks field for the slab - we only
	 * need to do that if there the block had that number of free chunks
	 * before we freed one. In that case, we check if there still are blocks
	 * in the original freelist and we either keep the current value (if there
	 * still are blocks) or increment it by one (the new block is still the
	 * one with minimum free chunks).
	 *
	 * The one exception is when the block will get completely free - in that
	 * case we will free it, se we can't use it for minFreeChunks. It however
	 * means there are no more blocks with free chunks.
	 */
	if (slab->minFreeChunks == (block->nfree - 1))
	{
		/* Have we removed the last chunk from the freelist? */
		if (dlist_is_empty(&slab->freelist[slab->minFreeChunks]))
		{
			/* but if we made the block entirely free, we'll free it */
			if (block->nfree == slab->chunksPerBlock)
				slab->minFreeChunks = 0;
			else
				slab->minFreeChunks++;
		}
	}

	/* If the block is now completely empty, free it. */
	if (block->nfree == slab->chunksPerBlock)
	{
		free(block);
		slab->nblocks--;
		slab->header.mem_allocated -= slab->blockSize;
	}
	else
		dlist_push_head(&slab->freelist[block->nfree], &block->node);

	Assert(slab->nblocks >= 0);
	Assert(slab->nblocks * slab->blockSize == slab->header.mem_allocated);
}

/*
 * SlabRealloc
 *		Change the allocated size of a chunk.
 *
 * As Slab is designed for allocating equally-sized chunks of memory, it can't
 * do an actual chunk size change.  We try to be gentle and allow calls with
 * exactly the same size, as in that case we can simply return the same
 * chunk.  When the size differs, we throw an error.
 *
 * We could also allow requests with size < chunkSize.  That however seems
 * rather pointless - Slab is meant for chunks of constant size, and moreover
 * realloc is usually used to enlarge the chunk.
 */
void *
SlabRealloc(void *pointer, Size size)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block = MemoryChunkGetBlock(chunk);
	SlabContext *slab = block->slab;

	Assert(slab);
	/* can't do actual realloc with slab, but let's try to be gentle */
	if (size == slab->chunkSize)
		return pointer;

	elog(ERROR, "slab allocator does not support realloc()");
	return NULL;				/* keep compiler quiet */
}

/*
 * SlabGetChunkContext
 *		Return the MemoryContext that 'pointer' belongs to.
 */
MemoryContext
SlabGetChunkContext(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block = MemoryChunkGetBlock(chunk);
	SlabContext *slab = block->slab;

	Assert(slab != NULL);

	return &slab->header;
}

/*
 * SlabGetChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
Size
SlabGetChunkSpace(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block = MemoryChunkGetBlock(chunk);
	SlabContext *slab = block->slab;

	Assert(slab);

	return slab->fullChunkSize;
}

/*
 * SlabIsEmpty
 *		Is an Slab empty of any allocated space?
 */
bool
SlabIsEmpty(MemoryContext context)
{
	SlabContext *slab = castNode(SlabContext, context);

	Assert(slab);

	return (slab->nblocks == 0);
}

/*
 * SlabStats
 *		Compute stats about memory consumption of a Slab context.
 *
 * printfunc: if not NULL, pass a human-readable stats string to this.
 * passthru: pass this pointer through to printfunc.
 * totals: if not NULL, add stats about this context into *totals.
 * print_to_stderr: print stats to stderr if true, elog otherwise.
 */
void
SlabStats(MemoryContext context,
		  MemoryStatsPrintFunc printfunc, void *passthru,
		  MemoryContextCounters *totals,
		  bool print_to_stderr)
{
	SlabContext *slab = castNode(SlabContext, context);
	Size		nblocks = 0;
	Size		freechunks = 0;
	Size		totalspace;
	Size		freespace = 0;
	int			i;

	/* Include context header in totalspace */
	totalspace = slab->headerSize;

	for (i = 0; i <= slab->chunksPerBlock; i++)
	{
		dlist_iter	iter;

		dlist_foreach(iter, &slab->freelist[i])
		{
			SlabBlock  *block = dlist_container(SlabBlock, node, iter.cur);

			nblocks++;
			totalspace += slab->blockSize;
			freespace += slab->fullChunkSize * block->nfree;
			freechunks += block->nfree;
		}
	}

	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu blocks; %zu free (%zu chunks); %zu used",
				 totalspace, nblocks, freespace, freechunks,
				 totalspace - freespace);
		printfunc(context, passthru, stats_string, print_to_stderr);
	}

	if (totals)
	{
		totals->nblocks += nblocks;
		totals->freechunks += freechunks;
		totals->totalspace += totalspace;
		totals->freespace += freespace;
	}
}


#ifdef MEMORY_CONTEXT_CHECKING

/*
 * SlabCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */
void
SlabCheck(MemoryContext context)
{
	int			i;
	SlabContext *slab = castNode(SlabContext, context);
	const char *name = slab->header.name;

	Assert(slab);
	Assert(slab->chunksPerBlock > 0);

	/* walk all the freelists */
	for (i = 0; i <= slab->chunksPerBlock; i++)
	{
		int			j,
					nfree;
		dlist_iter	iter;

		/* walk all blocks on this freelist */
		dlist_foreach(iter, &slab->freelist[i])
		{
			int			idx;
			SlabBlock  *block = dlist_container(SlabBlock, node, iter.cur);

			/*
			 * Make sure the number of free chunks (in the block header)
			 * matches position in the freelist.
			 */
			if (block->nfree != i)
				elog(WARNING, "problem in slab %s: number of free chunks %d in block %p does not match freelist %d",
					 name, block->nfree, block, i);

			/* make sure the slab pointer correctly points to this context */
			if (block->slab != slab)
				elog(WARNING, "problem in slab %s: bogus slab link in block %p",
					 name, block);

			/* reset the bitmap of free chunks for this block */
			memset(slab->freechunks, 0, (slab->chunksPerBlock * sizeof(bool)));
			idx = block->firstFreeChunk;

			/*
			 * Now walk through the chunks, count the free ones and also
			 * perform some additional checks for the used ones. As the chunk
			 * freelist is stored within the chunks themselves, we have to
			 * walk through the chunks and construct our own bitmap.
			 */

			nfree = 0;
			while (idx < slab->chunksPerBlock)
			{
				MemoryChunk *chunk;

				/* count the chunk as free, add it to the bitmap */
				nfree++;
				slab->freechunks[idx] = true;

				/* read index of the next free chunk */
				chunk = SlabBlockGetChunk(slab, block, idx);
				VALGRIND_MAKE_MEM_DEFINED(MemoryChunkGetPointer(chunk), sizeof(int32));
				idx = *(int32 *) MemoryChunkGetPointer(chunk);
			}

			for (j = 0; j < slab->chunksPerBlock; j++)
			{
				/* non-zero bit in the bitmap means chunk the chunk is used */
				if (!slab->freechunks[j])
				{
					MemoryChunk *chunk = SlabBlockGetChunk(slab, block, j);
					SlabBlock  *chunkblock = (SlabBlock *) MemoryChunkGetBlock(chunk);

					/*
					 * check the chunk's blockoffset correctly points back to
					 * the block
					 */
					if (chunkblock != block)
						elog(WARNING, "problem in slab %s: bogus block link in block %p, chunk %p",
							 name, block, chunk);

					/* there might be sentinel (thanks to alignment) */
					if (slab->chunkSize < (slab->fullChunkSize - Slab_CHUNKHDRSZ))
						if (!sentinel_ok(chunk, slab->chunkSize))
							elog(WARNING, "problem in slab %s: detected write past chunk end in block %p, chunk %p",
								 name, block, chunk);
				}
			}

			/*
			 * Make sure we got the expected number of free chunks (as tracked
			 * in the block header).
			 */
			if (nfree != block->nfree)
				elog(WARNING, "problem in slab %s: number of free chunks %d in block %p does not match bitmap %d",
					 name, block->nfree, block, nfree);
		}
	}

	Assert(slab->nblocks * slab->blockSize == context->mem_allocated);
}

#endif							/* MEMORY_CONTEXT_CHECKING */
