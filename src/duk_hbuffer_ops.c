/*
 *  duk_hbuffer operations such as resizing and inserting/appending data to
 *  a dynamic buffer.
 *
 *  Append operations append to the end of the buffer and they are relatively
 *  efficient: the buffer is grown with a "spare" part relative to the buffer
 *  size to minimize reallocations.  Insert operations need to move existing
 *  data forward in the buffer with memmove() and are not very efficient.
 *  They are used e.g. by the regexp compiler to "backpatch" regexp bytecode.
 */

#include "duk_internal.h"

/*
 *  Resizing
 */

DUK_INTERNAL void duk_hbuffer_resize(duk_hthread *thr, duk_hbuffer_dynamic *buf, duk_size_t new_size) {
	void *res;
	duk_size_t prev_size;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(buf != NULL);
	DUK_ASSERT(DUK_HBUFFER_HAS_DYNAMIC(buf));

	/*
	 *  Maximum size check
	 */

	if (new_size > DUK_HBUFFER_MAX_BYTELEN) {
		DUK_ERROR(thr, DUK_ERR_RANGE_ERROR, "buffer too long");
	}

	/*
	 *  Note: use indirect realloc variant just in case mark-and-sweep
	 *  (finalizers) might resize this same buffer during garbage
	 *  collection.
	 */

	res = DUK_REALLOC_INDIRECT(thr->heap, duk_hbuffer_get_dynalloc_ptr, (void *) buf, new_size);
	if (res != NULL || new_size == 0) {
		/* 'res' may be NULL if new allocation size is 0. */

		DUK_DDD(DUK_DDDPRINT("resized dynamic buffer %p:%ld -> %p:%ld",
		                     (void *) DUK_HBUFFER_DYNAMIC_GET_DATA_PTR(thr->heap, buf),
		                     (long) DUK_HBUFFER_DYNAMIC_GET_SIZE(buf),
		                     (void *) res,
		                     (long) new_size));

		/*
		 *  The entire allocated buffer area, regardless of actual used
		 *  size, is kept zeroed in resizes for simplicity.  If the buffer
		 *  is grown, zero the new part.
		 */

		prev_size = DUK_HBUFFER_DYNAMIC_GET_SIZE(buf);
		if (new_size > prev_size) {
			DUK_ASSERT(new_size - prev_size > 0);
#ifdef DUK_USE_ZERO_BUFFER_DATA
			DUK_MEMZERO((void *) ((char *) res + prev_size),
			            (duk_size_t) (new_size - prev_size));
#endif
		}

		DUK_HBUFFER_DYNAMIC_SET_SIZE(buf, new_size);
		DUK_HBUFFER_DYNAMIC_SET_DATA_PTR(thr->heap, buf, res);
	} else {
		DUK_ERROR(thr, DUK_ERR_ALLOC_ERROR, "buffer resize failed: %ld to %ld",
		          (long) DUK_HBUFFER_DYNAMIC_GET_SIZE(buf),
		          (long) new_size);
	}

	DUK_ASSERT(res != NULL || new_size == 0);
}

DUK_INTERNAL void duk_hbuffer_reset(duk_hthread *thr, duk_hbuffer_dynamic *buf) {
	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(buf != NULL);
	DUK_ASSERT(DUK_HBUFFER_HAS_DYNAMIC(buf));

	duk_hbuffer_resize(thr, buf, 0);
}
