#include "duk_internal.h"

DUK_INTERNAL duk_uint_t duk_hbufferobject_clamp_bytelength(duk_hbufferobject *h_bufobj, duk_uint_t len) {
	duk_uint_t buf_size;
	duk_uint_t buf_avail;

	DUK_ASSERT(h_bufobj != NULL);
	DUK_ASSERT(h_bufobj->buf != NULL);

	buf_size = (duk_uint_t) DUK_HBUFFER_GET_SIZE(h_bufobj->buf);
	if (h_bufobj->offset > buf_size) {
		/* Slice starting point is beyond current length. */
		return 0;
	}
	buf_avail = buf_size - h_bufobj->offset;

	return buf_avail >= len ? len : buf_avail;
}
