/* 
 * Two Levels Segregate Fit memory allocator (TLSF)
 * Version 2.3.2
 *
 * Written by Miguel Masmano Tello <mimastel@doctor.upv.es>
 *
 * Thanks to Ismael Ripoll for his suggestions and reviews
 *
 * Copyright (C) 2007, 2006, 2005, 2004
 *
 * This code is released using a dual license strategy: GPL/LGPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of the GNU General Public License Version 2.0
 * Released under the terms of the GNU Lesser General Public License Version 2.1
 *
 * See CREDITS for information on additional code contributions.
 */

//#include <linux/module.h>
//#include <linux/kernel.h>

#include <stdio.h>
//#include <string.h>	also gives ffs etc.

#include "tlsf.h"

/*************************************************************************/
/* Definition of the structures used by TLSF */


/* Some IMPORTANT TLSF parameters */
/* Unlike the preview TLSF versions, now they are statics */
#define MAX_FLI		(30)
#define MAX_LOG2_SLI	(5)
#define MAX_SLI		(1 << MAX_LOG2_SLI)	/* MAX_SLI = 2^MAX_LOG2_SLI */

#define FLI_OFFSET	(6) /* tlsf structure just will manage blocks bigger */
/* than 128 bytes */
#define SMALL_BLOCK	(128)
#define REAL_FLI	(MAX_FLI - FLI_OFFSET)
#define MIN_BLOCK_SIZE	(sizeof (free_ptr_t))
#define BHDR_OVERHEAD	(sizeof (bhdr_t) - MIN_BLOCK_SIZE)

#define	PTR_MASK	(sizeof(void *) - 1)
#define BLOCK_SIZE	(0xFFFFFFFF - PTR_MASK)

#define GET_NEXT_BLOCK(_addr, _r)	((bhdr_t *) ((char *) (_addr) + (_r)))
#define	MEM_ALIGN		(sizeof(void *) * 2 - 1)
#define ROUNDUP_SIZE(_r)	(((_r) + MEM_ALIGN) & ~MEM_ALIGN)
#define ROUNDDOWN_SIZE(_r)	((_r) & ~MEM_ALIGN)

#define BLOCK_STATE	(0x1)
#define PREV_STATE	(0x2)

/* bit 0 of the block size */
#define FREE_BLOCK	(0x1)
#define USED_BLOCK	(0x0)

/* bit 1 of the block size */
#define PREV_FREE	(0x2)
#define PREV_USED	(0x0)

#define ERR_PREFIX	"tlsf: "
#define ERR(fmt, args...) printf(ERR_PREFIX fmt "\n", ## args)

#define LOCK_PREFIX
#if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 1)
/* Technically wrong, but this avoids compilation errors on some gcc
   versions. */
#define ADDR "=m" (*(volatile long *) addr)
#else
#define ADDR "+m" (*(volatile long *) addr)
#endif

typedef unsigned int u32_t;
typedef unsigned char u8_t;

typedef struct free_ptr_struct {
	struct bhdr_struct *prev;
	struct bhdr_struct *next;
} free_ptr_t;

typedef struct bhdr_struct {
	/* This pointer is just valid if the first bit of size is set */
	struct bhdr_struct *prev_hdr;
	/* The size is stored in bytes */
	u32_t size; /* bit 0 indicates whether the block is used and */
		    /* bit 1 allows to know if the previous block is free */
	union {
		struct free_ptr_struct free_ptr;
		u8_t buffer[sizeof(struct free_ptr_struct)];
	} ptr;
} bhdr_t;

typedef struct pool_struct {
	/* the first-level bitmap */
	/* This array should have a size of REAL_FLI bits */
	u32_t fl_bitmap;

	/* the second-level bitmap */
	u32_t sl_bitmap[REAL_FLI];

	bhdr_t *matrix[REAL_FLI][MAX_SLI];

	size_t used_size;
	size_t init_size;
	size_t max_size;
	size_t grow_size;
	size_t num_regions;

	get_memory *get_mem;
	put_memory *put_mem;
} pool_t;

/**
 * fls - find last bit set
 * @x: the word to search
 *
 * This is defined the same way as ffs().
 */
static inline int fls(int x)
{
        int r;

        __asm__("bsrl %1,%0\n\t"
                "jnz 1f\n\t"
                "movl $-1,%0\n"
                "1:" : "=r" (r) : "rm" (x));
        return r+1;
}


/**
 * ffs - find first bit set
 * @x: the word to search
 *
 * This is defined the same way as
 * the libc and compiler builtin ffs routines, therefore
 * differs in spirit from the above ffz() (man ffs).
 */
static inline int ffs(int x)
{
        int r;

        __asm__("bsfl %1,%0\n\t"
                "jnz 1f\n\t"
                "movl $-1,%0\n"
                "1:" : "=r" (r) : "rm" (x));
        return r+1;
}


static inline void set_bit(int nr, volatile void *addr)
{
        asm volatile(LOCK_PREFIX "bts %1,%0"
                     : ADDR
                     : "Ir" (nr) : "memory");
}

static inline void clear_bit(int nr, volatile void *addr)
{
        asm volatile(LOCK_PREFIX "btr %1,%0"
                     : ADDR
                     : "Ir" (nr));
}

/******************************************************************/
/**************     Helping functions    **************************/
/******************************************************************/
static inline void MAPPING_SEARCH(size_t *_r, int *_fl, int *_sl)
{
	int _t;

	if (*_r < SMALL_BLOCK) {
		*_fl = 0;
		*_sl = *_r / (SMALL_BLOCK / MAX_SLI);
	} else {
		_t = (1 << (fls(*_r) - 1 - MAX_LOG2_SLI)) - 1;
		*_r = *_r + _t;
		*_fl = fls(*_r) - 1;
		*_sl = (*_r >> (*_fl - MAX_LOG2_SLI)) - MAX_SLI;
		*_fl -= FLI_OFFSET;
		/*if ((*_fl -= FLI_OFFSET) < 0) // FL wil be always >0!
		 *_fl = *_sl = 0;
		 */
		*_r &= ~_t;
	}
}

static inline void MAPPING_INSERT(size_t _r, int *_fl, int *_sl)
{
	if (_r < SMALL_BLOCK) {
		*_fl = 0;
		*_sl = _r / (SMALL_BLOCK / MAX_SLI);
	} else {
		*_fl = fls(_r) - 1;
		*_sl = (_r >> (*_fl - MAX_LOG2_SLI)) - MAX_SLI;
		*_fl -= FLI_OFFSET;
	}
}

static inline bhdr_t *FIND_SUITABLE_BLOCK(pool_t *_p, int *_fl,
								int *_sl) {
	u32_t _tmp = _p->sl_bitmap[*_fl] & (~0 << *_sl);
	bhdr_t *_b = NULL;

	if (_tmp) {
		*_sl = ffs(_tmp) - 1;
		_b = _p->matrix[*_fl][*_sl];
	} else {
		*_fl = ffs(_p->fl_bitmap & (~0 << (*_fl + 1))) - 1;
		if (*_fl > 0) {		/* likely */
			*_sl = ffs(_p->sl_bitmap[*_fl]) - 1;
			_b = _p->matrix[*_fl][*_sl];
		}
	}
	return _b;
}

static inline void EXTRACT_BLOCK_HDR(bhdr_t *_b, pool_t *_p, int _fl, int _sl)
{
	_p->matrix[_fl][_sl] = _b->ptr.free_ptr.next;
	if (_p->matrix[_fl][_sl])
		_p->matrix[_fl][_sl]->ptr.free_ptr.prev = NULL;
	else {
		clear_bit(_sl, (void *)&_p->sl_bitmap[_fl]);
		if(!_p->sl_bitmap[_fl])
			clear_bit (_fl, (void *)&_p->fl_bitmap);
	}
	_b->ptr.free_ptr = (free_ptr_t) {NULL, NULL};
}

static inline void EXTRACT_BLOCK(bhdr_t *_b, pool_t *_p, int _fl, int _sl)
{
	if (_b->ptr.free_ptr.next)
		_b->ptr.free_ptr.next->ptr.free_ptr.prev =
					_b->ptr.free_ptr.prev;
	if (_b->ptr.free_ptr.prev)
		_b->ptr.free_ptr.prev->ptr.free_ptr.next =
					_b->ptr.free_ptr.next;
	if (_p->matrix[_fl][_sl] == _b) {
		_p->matrix[_fl][_sl] = _b->ptr.free_ptr.next;
		if (!_p->matrix[_fl][_sl]) {
			clear_bit(_sl, (void *)&_p->sl_bitmap[_fl]);
			if (!_p->sl_bitmap[_fl])
				clear_bit (_fl, (void *)&_p->fl_bitmap);
		}
	}
	_b->ptr.free_ptr = (free_ptr_t) {NULL, NULL};
}

static inline void INSERT_BLOCK(bhdr_t *_b, pool_t *_p, int _fl, int _sl)
{
	_b->ptr.free_ptr = (free_ptr_t) {NULL, _p->matrix[_fl][_sl]};
	if (_p->matrix[_fl][_sl])
		_p->matrix[_fl][_sl]->ptr.free_ptr.prev = _b;
	_p->matrix[_fl][_sl] = _b;
	set_bit(_sl, (void *)&_p->sl_bitmap[_fl]);
	set_bit(_fl, (void *)&_p->fl_bitmap);
}

static inline void ADD_REGION(void *region, size_t region_size, pool_t *pool)
{
	int fl, sl;
	bhdr_t *b, *lb;

	// TODO: sanity checks

	b = (bhdr_t *)(region);
	b->prev_hdr = NULL;
	b->size = ROUNDDOWN_SIZE(region_size - 2 * BHDR_OVERHEAD)
						| FREE_BLOCK | PREV_USED;
	MAPPING_INSERT(b->size & BLOCK_SIZE, &fl, &sl);
	INSERT_BLOCK(b, pool, fl, sl);
	/* The sentinel block, it allow us to know when we're in the last block */
	lb = GET_NEXT_BLOCK(b->ptr.buffer, b->size & BLOCK_SIZE);
	lb->prev_hdr = b;
	lb->size = 0 | USED_BLOCK | PREV_FREE;
	pool->used_size += 2 * BHDR_OVERHEAD;
}

/******************************************************************/
/******************** Begin of the allocator code *****************/
/******************************************************************/
void *tlsf_create_memory_pool(get_memory get_mem,
			put_memory put_mem,
			size_t init_size,
			size_t max_size,
			size_t grow_size)
{
	pool_t *pool;
	void *region;
	int i;

	// TODO: sanity checks

	pool = get_mem(ROUNDUP_SIZE(sizeof(pool_t)));
	if (pool == NULL) {
		ERR("Error allocating pool structure.\n");
		goto out;
	}
	for (i = 0; i < sizeof(pool_t); i++)
		*((char *)(pool) + i) = 0;
	//memset(pool, 0x0, ROUNDUP_SIZE(sizeof(pool_t)));

	pool->used_size += ROUNDUP_SIZE(sizeof(pool_t));
	pool->init_size = init_size;
	pool->max_size = max_size;
	pool->grow_size = grow_size;
	pool->get_mem = get_mem;
	pool->put_mem = put_mem;

	region = get_mem(init_size);
	if (region == NULL) {
		ERR("Error allocating first region: size=%u\n", init_size);
		goto out_region;
	}
	ADD_REGION(region, init_size, pool);
	pool->num_regions++;

	return pool;

out_region:
	put_mem(pool);

out:
	return NULL;
}

size_t tlsf_get_used_size(void *mem_pool)
{
	pool_t *tlsf = (pool_t *)mem_pool;
	return tlsf->used_size;
}

size_t tlsf_get_total_size(void *mem_pool)
{
	pool_t *pool = (pool_t *)mem_pool;
	return (pool->init_size + (pool->num_regions - 1) * pool->grow_size);
}

void tlsf_destroy_memory_pool(void *mem_pool) 
{
	// TODO: check pool status and do cleanups

	pool_t *pool = (pool_t *)mem_pool;
	pool->put_mem(pool);
}

void *tlsf_malloc(size_t size, void *mem_pool)
{
    pool_t *tlsf = (pool_t *)mem_pool;
    bhdr_t *b, *b2, *next_b, *region;
    int fl, sl;
    size_t tmp_size;

    printf("tlsf kmod malloc called\n");

    size = (size < MIN_BLOCK_SIZE) ? MIN_BLOCK_SIZE : ROUNDUP_SIZE(size);
    /* Rounding up the requested size and calculating fl and sl */

retry_find:
    MAPPING_SEARCH(&size, &fl, &sl);

    /* Searching a free block */
    if (!(b = FIND_SUITABLE_BLOCK(tlsf, &fl, &sl))) {
		/* Not found */
		if (size > (tlsf->grow_size - 2 * BHDR_OVERHEAD)) {
			ERR("Impossible allocation request: size=%u", size);
			return NULL;
		}
		region = tlsf->get_mem(tlsf->grow_size);
		if (region == NULL) {
			ERR("Error allocating new region");
			return NULL;
		}
		ADD_REGION(region, tlsf->grow_size, tlsf);
		tlsf->num_regions++;

		goto retry_find;
    }
    EXTRACT_BLOCK_HDR(b, tlsf, fl, sl);

    /*-- found: */
    next_b = GET_NEXT_BLOCK(b->ptr.buffer, b->size & BLOCK_SIZE);
    /* Should the block be split? */
    tmp_size = (b->size & BLOCK_SIZE) - size;
    if (tmp_size >= sizeof (bhdr_t) ) {
		tmp_size -= BHDR_OVERHEAD;
		b2 = GET_NEXT_BLOCK(b->ptr.buffer, size);
		b2->size = tmp_size | FREE_BLOCK | PREV_USED;
		next_b->prev_hdr = b2;

		MAPPING_INSERT(tmp_size, &fl, &sl);
		INSERT_BLOCK(b2, tlsf, fl, sl);

		b->size = size | (b->size & PREV_STATE);
    } else {
		next_b->size &= (~PREV_FREE);
		b->size &= (~FREE_BLOCK);	/* Now it's used */
    }

    tlsf->used_size += (b->size & BLOCK_SIZE) + BHDR_OVERHEAD;

    return (void *) b->ptr.buffer;
}

void tlsf_free(void *ptr, void *mem_pool)
{
    pool_t *tlsf = (pool_t *) mem_pool;
    bhdr_t *b, *tmp_b;
    int fl = 0, sl = 0;

    if (ptr == NULL) {
	return;
    }
    b = (bhdr_t *) ((char *) ptr - BHDR_OVERHEAD);

    b->size |= FREE_BLOCK;
    tlsf->used_size -= (b->size & BLOCK_SIZE) + BHDR_OVERHEAD;
    b->ptr.free_ptr = (free_ptr_t) { NULL, NULL};
    tmp_b = GET_NEXT_BLOCK(b->ptr.buffer, b->size & BLOCK_SIZE);
    if (tmp_b->size & FREE_BLOCK) {
		MAPPING_INSERT(tmp_b->size & BLOCK_SIZE, &fl, &sl);
		EXTRACT_BLOCK(tmp_b, tlsf, fl, sl);
		b->size += (tmp_b->size & BLOCK_SIZE) + BHDR_OVERHEAD;
    }
    if (b->size & PREV_FREE) {
		tmp_b = b->prev_hdr;
		MAPPING_INSERT(tmp_b->size & BLOCK_SIZE, &fl, &sl);
		EXTRACT_BLOCK(tmp_b, tlsf, fl, sl);
		tmp_b->size += (b->size & BLOCK_SIZE) + BHDR_OVERHEAD;
		b = tmp_b;
    }
    tmp_b = GET_NEXT_BLOCK(b->ptr.buffer, b->size & BLOCK_SIZE);
    MAPPING_INSERT(b->size & BLOCK_SIZE, &fl, &sl);

    if (((b->size & BLOCK_SIZE) == (tlsf->grow_size - 2 * BHDR_OVERHEAD))
		&& ((tmp_b->size & BLOCK_SIZE) == 0)) {
	tlsf->put_mem(b);
	tlsf->num_regions--;
	return;
    }
    INSERT_BLOCK(b, tlsf, fl, sl);

    tmp_b->size |= PREV_FREE;
    tmp_b->prev_hdr = b;
}

void *tlsf_calloc(size_t nelem, size_t elem_size, void *mem_pool)
{
    void *ptr;

    if (nelem <= 0 || elem_size <= 0)
		return NULL;

    if (!(ptr = tlsf_malloc(nelem * elem_size, mem_pool)))
		return NULL;
    //memset(ptr, 0, nelem * elem_size);

    return ptr;
}

