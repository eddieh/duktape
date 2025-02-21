/*
 *  Ecmascript bytecode executor.
 */

#include "duk_internal.h"

/*
 *  Local declarations
 */

DUK_LOCAL_DECL void duk__reconfig_valstack(duk_hthread *thr, duk_size_t act_idx, duk_small_uint_t retval_count);

/*
 *  Arithmetic, binary, and logical helpers.
 *
 *  Note: there is no opcode for logical AND or logical OR; this is on
 *  purpose, because the evalution order semantics for them make such
 *  opcodes pretty pointless (short circuiting means they are most
 *  comfortably implemented as jumps).  However, a logical NOT opcode
 *  is useful.
 *
 *  Note: careful with duk_tval pointers here: they are potentially
 *  invalidated by any DECREF and almost any API call.
 */

DUK_LOCAL duk_double_t duk__compute_mod(duk_double_t d1, duk_double_t d2) {
	/*
	 *  Ecmascript modulus ('%') does not match IEEE 754 "remainder"
	 *  operation (implemented by remainder() in C99) but does seem
	 *  to match ANSI C fmod().
	 *
	 *  Compare E5 Section 11.5.3 and "man fmod".
	 */

	return (duk_double_t) DUK_FMOD((double) d1, (double) d2);
}

DUK_LOCAL void duk__vm_arith_add(duk_hthread *thr, duk_tval *tv_x, duk_tval *tv_y, duk_small_uint_fast_t idx_z) {
	/*
	 *  Addition operator is different from other arithmetic
	 *  operations in that it also provides string concatenation.
	 *  Hence it is implemented separately.
	 *
	 *  There is a fast path for number addition.  Other cases go
	 *  through potentially multiple coercions as described in the
	 *  E5 specification.  It may be possible to reduce the number
	 *  of coercions, but this must be done carefully to preserve
	 *  the exact semantics.
	 *
	 *  E5 Section 11.6.1.
	 *
	 *  Custom types also have special behavior implemented here.
	 */

	duk_context *ctx = (duk_context *) thr;
	duk_double_union du;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(tv_x != NULL);  /* may be reg or const */
	DUK_ASSERT(tv_y != NULL);  /* may be reg or const */
	DUK_ASSERT_DISABLE(idx_z >= 0);  /* unsigned */
	DUK_ASSERT((duk_uint_t) idx_z < (duk_uint_t) duk_get_top(ctx));

	/*
	 *  Fast paths
	 */

#if defined(DUK_USE_FASTINT)
	if (DUK_TVAL_IS_FASTINT(tv_x) && DUK_TVAL_IS_FASTINT(tv_y)) {
		duk_int64_t v1, v2, v3;
		duk_int32_t v3_hi;
		duk_tval tv_tmp;
		duk_tval *tv_z;

		/* Input values are signed 48-bit so we can detect overflow
		 * reliably from high bits or just a comparison.
		 */

		v1 = DUK_TVAL_GET_FASTINT(tv_x);
		v2 = DUK_TVAL_GET_FASTINT(tv_y);
		v3 = v1 + v2;
		v3_hi = (duk_int32_t) (v3 >> 32);
		if (DUK_LIKELY(v3_hi >= -0x8000LL && v3_hi <= 0x7fffLL)) {
			tv_z = thr->valstack_bottom + idx_z;
			DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
			DUK_TVAL_SET_FASTINT(tv_z, v3);
			DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
			DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
			return;
		} else {
			/* overflow, fall through */
			;
		}
	}
#endif  /* DUK_USE_FASTINT */

	if (DUK_TVAL_IS_NUMBER(tv_x) && DUK_TVAL_IS_NUMBER(tv_y)) {
		duk_tval tv_tmp;
		duk_tval *tv_z;

		du.d = DUK_TVAL_GET_NUMBER(tv_x) + DUK_TVAL_GET_NUMBER(tv_y);
		DUK_DBLUNION_NORMALIZE_NAN_CHECK(&du);
		DUK_ASSERT(DUK_DBLUNION_IS_NORMALIZED(&du));

		tv_z = thr->valstack_bottom + idx_z;
		DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
		DUK_TVAL_SET_NUMBER(tv_z, du.d);
		DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
		DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
		return;
	}

	/*
	 *  Slow path: potentially requires function calls for coercion
	 */

	duk_push_tval(ctx, tv_x);
	duk_push_tval(ctx, tv_y);
	duk_to_primitive(ctx, -2, DUK_HINT_NONE);  /* side effects -> don't use tv_x, tv_y after */
	duk_to_primitive(ctx, -1, DUK_HINT_NONE);

	/* As a first approximation, buffer values are coerced to strings
	 * for addition.  This means that adding two buffers currently
	 * results in a string.
	 */
	if (duk_check_type_mask(ctx, -2, DUK_TYPE_MASK_STRING | DUK_TYPE_MASK_BUFFER) ||
	    duk_check_type_mask(ctx, -1, DUK_TYPE_MASK_STRING | DUK_TYPE_MASK_BUFFER)) {
		duk_to_string(ctx, -2);
		duk_to_string(ctx, -1);
		duk_concat(ctx, 2);  /* [... s1 s2] -> [... s1+s2] */
		duk_replace(ctx, (duk_idx_t) idx_z);  /* side effects */
	} else {
		duk_double_t d1, d2;

		d1 = duk_to_number(ctx, -2);
		d2 = duk_to_number(ctx, -1);
		DUK_ASSERT(duk_is_number(ctx, -2));
		DUK_ASSERT(duk_is_number(ctx, -1));
		DUK_ASSERT_DOUBLE_IS_NORMALIZED(d1);
		DUK_ASSERT_DOUBLE_IS_NORMALIZED(d2);

		du.d = d1 + d2;
		DUK_DBLUNION_NORMALIZE_NAN_CHECK(&du);
		DUK_ASSERT(DUK_DBLUNION_IS_NORMALIZED(&du));

		duk_pop_2(ctx);
		duk_push_number(ctx, du.d);
		duk_replace(ctx, (duk_idx_t) idx_z);  /* side effects */
	}
}

DUK_LOCAL void duk__vm_arith_binary_op(duk_hthread *thr, duk_tval *tv_x, duk_tval *tv_y, duk_idx_t idx_z, duk_small_uint_fast_t opcode) {
	/*
	 *  Arithmetic operations other than '+' have number-only semantics
	 *  and are implemented here.  The separate switch-case here means a
	 *  "double dispatch" of the arithmetic opcode, but saves code space.
	 *
	 *  E5 Sections 11.5, 11.5.1, 11.5.2, 11.5.3, 11.6, 11.6.1, 11.6.2, 11.6.3.
	 */

	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_tmp;
	duk_tval *tv_z;
	duk_double_t d1, d2;
	duk_double_union du;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(tv_x != NULL);  /* may be reg or const */
	DUK_ASSERT(tv_y != NULL);  /* may be reg or const */
	DUK_ASSERT_DISABLE(idx_z >= 0);  /* unsigned */
	DUK_ASSERT((duk_uint_t) idx_z < (duk_uint_t) duk_get_top(ctx));

#if defined(DUK_USE_FASTINT)
	if (DUK_TVAL_IS_FASTINT(tv_x) && DUK_TVAL_IS_FASTINT(tv_y)) {
		duk_int64_t v1, v2, v3;
		duk_int32_t v3_hi;

		v1 = DUK_TVAL_GET_FASTINT(tv_x);
		v2 = DUK_TVAL_GET_FASTINT(tv_y);

		switch (opcode) {
		case DUK_OP_SUB: {
			v3 = v1 - v2;
			break;
		}
		case DUK_OP_MUL: {
			/* Must ensure result is 64-bit (no overflow); a
			 * simple and sufficient fast path is to allow only
			 * 32-bit inputs.  Avoid zero inputs to avoid
			 * negative zero issues (-1 * 0 = -0, for instance).
			 */
			if (v1 >= -0x80000000LL && v1 <= 0x7fffffffLL && v1 != 0 &&
			    v2 >= -0x80000000LL && v2 <= 0x7fffffffLL && v2 != 0) {
				v3 = v1 * v2;
			} else {
				goto skip_fastint;
			}
			break;
		}
		case DUK_OP_DIV: {
			/* Don't allow a zero divisor.  Fast path check by
			 * "verifying" with multiplication.  Also avoid zero
			 * dividend to avoid negative zero issues (0 / -1 = -0
			 * for instance).
			 */
			if (v1 == 0 || v2 == 0) {
				goto skip_fastint;
			}
			v3 = v1 / v2;
			if (v3 * v2 != v1) {
				goto skip_fastint;
			}
			break;
		}
		case DUK_OP_MOD: {
			/* Don't allow a zero divisor.  Restrict both v1 and
			 * v2 to positive values to avoid compiler specific
			 * behavior.
			 */
			if (v1 < 1 || v2 < 1) {
				goto skip_fastint;
			}
			v3 = v1 % v2;
			DUK_ASSERT(v3 >= 0);
			DUK_ASSERT(v3 < v2);
			DUK_ASSERT(v1 - (v1 / v2) * v2 == v3);
			break;
		}
		default: {
			DUK_UNREACHABLE();
			goto skip_fastint;
		}
		}

		v3_hi = (duk_int32_t) (v3 >> 32);
		if (DUK_LIKELY(v3_hi >= -0x8000LL && v3_hi <= 0x7fffLL)) {
			tv_z = thr->valstack_bottom + idx_z;
			DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
			DUK_TVAL_SET_FASTINT(tv_z, v3);
			DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
			DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
			return;
		}
		/* fall through if overflow etc */
	}
 skip_fastint:
#endif  /* DUK_USE_FASTINT */

	if (DUK_TVAL_IS_NUMBER(tv_x) && DUK_TVAL_IS_NUMBER(tv_y)) {
		/* fast path */
		d1 = DUK_TVAL_GET_NUMBER(tv_x);
		d2 = DUK_TVAL_GET_NUMBER(tv_y);
	} else {
		duk_push_tval(ctx, tv_x);
		duk_push_tval(ctx, tv_y);
		d1 = duk_to_number(ctx, -2);  /* side effects */
		d2 = duk_to_number(ctx, -1);
		DUK_ASSERT(duk_is_number(ctx, -2));
		DUK_ASSERT(duk_is_number(ctx, -1));
		DUK_ASSERT_DOUBLE_IS_NORMALIZED(d1);
		DUK_ASSERT_DOUBLE_IS_NORMALIZED(d2);
		duk_pop_2(ctx);
	}

	switch (opcode) {
	case DUK_OP_SUB: {
		du.d = d1 - d2;
		break;
	}
	case DUK_OP_MUL: {
		du.d = d1 * d2;
		break;
	}
	case DUK_OP_DIV: {
		du.d = d1 / d2;
		break;
	}
	case DUK_OP_MOD: {
		du.d = duk__compute_mod(d1, d2);
		break;
	}
	default: {
		DUK_UNREACHABLE();
		du.d = DUK_DOUBLE_NAN;  /* should not happen */
		break;
	}
	}

	/* important to use normalized NaN with 8-byte tagged types */
	DUK_DBLUNION_NORMALIZE_NAN_CHECK(&du);
	DUK_ASSERT(DUK_DBLUNION_IS_NORMALIZED(&du));

	tv_z = thr->valstack_bottom + idx_z;
	DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
	DUK_TVAL_SET_NUMBER(tv_z, du.d);
	DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
}

DUK_LOCAL void duk__vm_bitwise_binary_op(duk_hthread *thr, duk_tval *tv_x, duk_tval *tv_y, duk_small_uint_fast_t idx_z, duk_small_uint_fast_t opcode) {
	/*
	 *  Binary bitwise operations use different coercions (ToInt32, ToUint32)
	 *  depending on the operation.  We coerce the arguments first using
	 *  ToInt32(), and then cast to an 32-bit value if necessary.  Note that
	 *  such casts must be correct even if there is no native 32-bit type
	 *  (e.g., duk_int32_t and duk_uint32_t are 64-bit).
	 *
	 *  E5 Sections 11.10, 11.7.1, 11.7.2, 11.7.3
	 */

	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_tmp;
	duk_tval *tv_z;
	duk_int32_t i1, i2, i3;
	duk_uint32_t u1, u2, u3;
#if defined(DUK_USE_FASTINT)
	duk_int64_t fi3;
#else
	duk_double_t d3;
#endif

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(tv_x != NULL);  /* may be reg or const */
	DUK_ASSERT(tv_y != NULL);  /* may be reg or const */
	DUK_ASSERT_DISABLE(idx_z >= 0);  /* unsigned */
	DUK_ASSERT((duk_uint_t) idx_z < (duk_uint_t) duk_get_top(ctx));

#if defined(DUK_USE_FASTINT)
	if (DUK_TVAL_IS_FASTINT(tv_x) && DUK_TVAL_IS_FASTINT(tv_y)) {
		i1 = (duk_int32_t) DUK_TVAL_GET_FASTINT_I32(tv_x);
		i2 = (duk_int32_t) DUK_TVAL_GET_FASTINT_I32(tv_y);
	}
	else
#endif  /* DUK_USE_FASTINT */
	{
		duk_push_tval(ctx, tv_x);
		duk_push_tval(ctx, tv_y);
		i1 = duk_to_int32(ctx, -2);
		i2 = duk_to_int32(ctx, -1);
		duk_pop_2(ctx);
	}

	switch (opcode) {
	case DUK_OP_BAND: {
		i3 = i1 & i2;
		break;
	}
	case DUK_OP_BOR: {
		i3 = i1 | i2;
		break;
	}
	case DUK_OP_BXOR: {
		i3 = i1 ^ i2;
		break;
	}
	case DUK_OP_BASL: {
		/* Signed shift, named "arithmetic" (asl) because the result
		 * is signed, e.g. 4294967295 << 1 -> -2.  Note that result
		 * must be masked.
		 */

		u2 = ((duk_uint32_t) i2) & 0xffffffffUL;
		i3 = i1 << (u2 & 0x1f);                      /* E5 Section 11.7.1, steps 7 and 8 */
		i3 = i3 & ((duk_int32_t) 0xffffffffUL);      /* Note: left shift, should mask */
		break;
	}
	case DUK_OP_BASR: {
		/* signed shift */

		u2 = ((duk_uint32_t) i2) & 0xffffffffUL;
		i3 = i1 >> (u2 & 0x1f);                      /* E5 Section 11.7.2, steps 7 and 8 */
		break;
	}
	case DUK_OP_BLSR: {
		/* unsigned shift */

		u1 = ((duk_uint32_t) i1) & 0xffffffffUL;
		u2 = ((duk_uint32_t) i2) & 0xffffffffUL;

		/* special result value handling */
		u3 = u1 >> (u2 & 0x1f);     /* E5 Section 11.7.2, steps 7 and 8 */
#if defined(DUK_USE_FASTINT)
		fi3 = (duk_int64_t) u3;
		goto fastint_result_set;
#else
		d3 = (duk_double_t) u3;
		goto result_set;
#endif
	}
	default: {
		DUK_UNREACHABLE();
		i3 = 0;  /* should not happen */
		break;
	}
	}

#if defined(DUK_USE_FASTINT)
	/* Result is always fastint compatible. */
	/* XXX: set 32-bit result (but must handle signed and unsigned) */
	fi3 = (duk_int64_t) i3;

 fastint_result_set:
	tv_z = thr->valstack_bottom + idx_z;
	DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
	DUK_TVAL_SET_FASTINT(tv_z, fi3);
	DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
#else
	d3 = (duk_double_t) i3;

 result_set:
	DUK_ASSERT(!DUK_ISNAN(d3));            /* 'd3' is never NaN, so no need to normalize */
	DUK_ASSERT_DOUBLE_IS_NORMALIZED(d3);   /* always normalized */

	tv_z = thr->valstack_bottom + idx_z;
	DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
	DUK_TVAL_SET_NUMBER(tv_z, d3);
	DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
#endif
}

/* In-place unary operation. */
DUK_LOCAL void duk__vm_arith_unary_op(duk_hthread *thr, duk_tval *tv_x, duk_idx_t idx_x, duk_small_uint_fast_t opcode) {
	/*
	 *  Arithmetic operations other than '+' have number-only semantics
	 *  and are implemented here.  The separate switch-case here means a
	 *  "double dispatch" of the arithmetic opcode, but saves code space.
	 *
	 *  E5 Sections 11.5, 11.5.1, 11.5.2, 11.5.3, 11.6, 11.6.1, 11.6.2, 11.6.3.
	 */

	duk_context *ctx = (duk_context *) thr;
	duk_double_t d1;
	duk_double_union du;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(opcode == DUK_EXTRAOP_UNM || opcode == DUK_EXTRAOP_UNP);

#if defined(DUK_USE_FASTINT)
	if (DUK_TVAL_IS_FASTINT(tv_x)) {
		duk_int64_t v1, v2;

		v1 = DUK_TVAL_GET_FASTINT(tv_x);
		if (opcode == DUK_EXTRAOP_UNM) {
			/* The smallest fastint is no longer 48-bit when
			 * negated.  Positive zero becames negative zero
			 * (cannot be represented) when negated.
			 */
			if (DUK_LIKELY(v1 != DUK_FASTINT_MIN && v1 != 0)) {
				v2 = -v1;
				DUK_TVAL_SET_FASTINT(tv_x, v2);  /* no refcount changes */
				return;
			}
		} else {
			/* ToNumber() for a fastint is a no-op. */
			DUK_ASSERT(opcode == DUK_EXTRAOP_UNP);
			return;
		}
		/* fall through if overflow etc */
	}
#endif  /* DUK_USE_FASTINT */

	if (!DUK_TVAL_IS_NUMBER(tv_x)) {
		duk_to_number(ctx, idx_x);  /* side effects, perform in-place */
		tv_x = duk_get_tval(ctx, idx_x);
		DUK_ASSERT(tv_x != NULL);
		DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_x));
	}

	d1 = DUK_TVAL_GET_NUMBER(tv_x);
	if (opcode == DUK_EXTRAOP_UNM) {
		du.d = -d1;
	} else {
		/* ToNumber() for a double is a no-op. */
		DUK_ASSERT(opcode == DUK_EXTRAOP_UNP);
		du.d = d1;
	}
	DUK_DBLUNION_NORMALIZE_NAN_CHECK(&du);  /* mandatory if du.d is a NaN */

	DUK_ASSERT(DUK_DBLUNION_IS_NORMALIZED(&du));

#if defined(DUK_USE_FASTINT)
	/* Unary plus is used to force a fastint check, so must include
	 * downgrade check.
	 */
	DUK_TVAL_SET_NUMBER_CHKFAST(tv_x, du.d);  /* no refcount changes */
#else
	DUK_TVAL_SET_NUMBER(tv_x, du.d);  /* no refcount changes */
#endif
}

DUK_LOCAL void duk__vm_bitwise_not(duk_hthread *thr, duk_tval *tv_x, duk_small_uint_fast_t idx_z) {
	/*
	 *  E5 Section 11.4.8
	 */

	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_tmp;
	duk_tval *tv_z;
	duk_int32_t i1, i2;
#if !defined(DUK_USE_FASTINT)
	duk_double_t d2;
#endif

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(tv_x != NULL);  /* may be reg or const */
	DUK_ASSERT_DISABLE(idx_z >= 0);
	DUK_ASSERT((duk_uint_t) idx_z < (duk_uint_t) duk_get_top(ctx));

#if defined(DUK_USE_FASTINT)
	if (DUK_TVAL_IS_FASTINT(tv_x)) {
		i1 = (duk_int32_t) DUK_TVAL_GET_FASTINT_I32(tv_x);
	}
	else
#endif  /* DUK_USE_FASTINT */
	{
		duk_push_tval(ctx, tv_x);
		i1 = duk_to_int32(ctx, -1);
		duk_pop(ctx);
	}

	i2 = ~i1;

#if defined(DUK_USE_FASTINT)
	/* Result is always fastint compatible. */
	tv_z = thr->valstack_bottom + idx_z;
	DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
	DUK_TVAL_SET_FASTINT_I32(tv_z, i2);
	DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
#else
	d2 = (duk_double_t) i2;

	DUK_ASSERT(!DUK_ISNAN(d2));            /* 'val' is never NaN, so no need to normalize */
	DUK_ASSERT_DOUBLE_IS_NORMALIZED(d2);   /* always normalized */

	tv_z = thr->valstack_bottom + idx_z;
	DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
	DUK_TVAL_SET_NUMBER(tv_z, d2);
	DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
#endif
}

DUK_LOCAL void duk__vm_logical_not(duk_hthread *thr, duk_tval *tv_x, duk_tval *tv_z) {
	/*
	 *  E5 Section 11.4.9
	 */

	duk_tval tv_tmp;
	duk_bool_t res;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(tv_x != NULL);  /* may be reg or const */
	DUK_ASSERT(tv_z != NULL);  /* reg */

	DUK_UNREF(thr);  /* w/o refcounts */

	/* ToBoolean() does not require any operations with side effects so
	 * we can do it efficiently.  For footprint it would be better to use
	 * duk_js_toboolean() and then push+replace to the result slot.
	 */
	res = duk_js_toboolean(tv_x);  /* does not modify tv_x */
	DUK_ASSERT(res == 0 || res == 1);
	res ^= 1;
	DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
	DUK_TVAL_SET_BOOLEAN(tv_z, res);  /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
}

/*
 *  Longjmp handler for the bytecode executor (and a bunch of static
 *  helpers for it).
 *
 *  Any type of longjmp() can be caught here, including intra-function
 *  longjmp()s like 'break', 'continue', (slow) 'return', 'yield', etc.
 *
 *  Error policy: should not ordinarily throw errors.  Errors thrown
 *  will bubble outwards.
 *
 *  Returns:
 *    0   restart execution
 *    1   bytecode executor finished
 *    2   rethrow longjmp
 */

/* XXX: duk_api operations for cross-thread reg manipulation? */
/* XXX: post-condition: value stack must be correct; for ecmascript functions, clamped to 'nregs' */

#define DUK__LONGJMP_RESTART   0  /* state updated, restart bytecode execution */
#define DUK__LONGJMP_FINISHED  1  /* exit bytecode executor with return value */
#define DUK__LONGJMP_RETHROW   2  /* exit bytecode executor by rethrowing an error to caller */

/* only called when act_idx points to an Ecmascript function */
DUK_LOCAL void duk__reconfig_valstack(duk_hthread *thr, duk_size_t act_idx, duk_small_uint_t retval_count) {
	duk_hcompiledfunction *h_func;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT_DISABLE(act_idx >= 0);  /* unsigned */
	DUK_ASSERT(DUK_ACT_GET_FUNC(thr->callstack + act_idx) != NULL);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(thr->callstack + act_idx)));
	DUK_ASSERT_DISABLE(thr->callstack[act_idx].idx_retval >= 0);  /* unsigned */

	thr->valstack_bottom = thr->valstack + thr->callstack[act_idx].idx_bottom;

	/* clamp so that retval is at the top (retval_count == 1) or register just before
	 * intended retval is at the top (retval_count == 0, happens e.g. with 'finally').
	 */
	duk_set_top((duk_context *) thr,
	            (duk_idx_t) (thr->callstack[act_idx].idx_retval -
	                         thr->callstack[act_idx].idx_bottom +
	                         retval_count));

	/*
	 *  When returning to an Ecmascript function, extend the valstack
	 *  top to 'nregs' always.
	 */

	h_func = (duk_hcompiledfunction *) DUK_ACT_GET_FUNC(thr->callstack + act_idx);

	(void) duk_valstack_resize_raw((duk_context *) thr,
	                               (thr->valstack_bottom - thr->valstack) +      /* bottom of current func */
	                                   h_func->nregs +                           /* reg count */
	                                   DUK_VALSTACK_INTERNAL_EXTRA,              /* + spare */
	                               DUK_VSRESIZE_FLAG_SHRINK |                    /* flags */
	                               0 /* no compact */ |
	                               DUK_VSRESIZE_FLAG_THROW);

	duk_set_top((duk_context *) thr, h_func->nregs);
}

DUK_LOCAL void duk__handle_catch_or_finally(duk_hthread *thr, duk_size_t cat_idx, duk_bool_t is_finally) {
	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_tmp;
	duk_tval *tv1;

	DUK_DDD(DUK_DDDPRINT("handling catch/finally, cat_idx=%ld, is_finally=%ld",
	                     (long) cat_idx, (long) is_finally));

	/*
	 *  Set caught value and longjmp type to catcher regs.
	 */

	DUK_DDD(DUK_DDDPRINT("writing catch registers: idx_base=%ld -> %!T, idx_base+1=%ld -> %!T",
	                     (long) thr->catchstack[cat_idx].idx_base,
	                     (duk_tval *) &thr->heap->lj.value1,
	                     (long) (thr->catchstack[cat_idx].idx_base + 1),
	                     (duk_tval *) &thr->heap->lj.value2));

	tv1 = thr->valstack + thr->catchstack[cat_idx].idx_base;
	DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
	DUK_TVAL_SET_TVAL(tv1, &thr->heap->lj.value1);
	DUK_TVAL_INCREF(thr, tv1);
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

	tv1 = thr->valstack + thr->catchstack[cat_idx].idx_base + 1;
	DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
	DUK_TVAL_SET_NUMBER(tv1, (duk_double_t) thr->heap->lj.type);  /* XXX: set int */
	DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv1));   /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

	/*
	 *  Unwind catchstack and callstack.
	 *
	 *  The 'cat_idx' catcher is always kept, even when executing finally.
	 */

	duk_hthread_catchstack_unwind(thr, cat_idx + 1);
	duk_hthread_callstack_unwind(thr, thr->catchstack[cat_idx].callstack_index + 1);

	/*
	 *  Reconfigure valstack to 'nregs' (this is always the case for
	 *  Ecmascript functions).
	 */

	DUK_ASSERT(thr->callstack_top >= 1);
	DUK_ASSERT(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 1) != NULL);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 1)));

	thr->valstack_bottom = thr->valstack + (thr->callstack + thr->callstack_top - 1)->idx_bottom;
	duk_set_top((duk_context *) thr, ((duk_hcompiledfunction *) DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 1))->nregs);

	/*
	 *  Reset PC: resume execution from catch or finally jump slot.
	 */

	(thr->callstack + thr->callstack_top - 1)->pc =
		thr->catchstack[cat_idx].pc_base + (is_finally ? 1 : 0);

	/*
	 *  If entering a 'catch' block which requires an automatic
	 *  catch variable binding, create the lexical environment.
	 *
	 *  The binding is mutable (= writable) but not deletable.
	 *  Step 4 for the catch production in E5 Section 12.14;
	 *  no value is given for CreateMutableBinding 'D' argument,
	 *  which implies the binding is not deletable.
	 */

	if (!is_finally && DUK_CAT_HAS_CATCH_BINDING_ENABLED(&thr->catchstack[cat_idx])) {
		duk_activation *act;
		duk_hobject *new_env;
		duk_hobject *act_lex_env;

		DUK_DDD(DUK_DDDPRINT("catcher has an automatic catch binding"));

		/* Note: 'act' is dangerous here because it may get invalidate at many
		 * points, so we re-lookup it multiple times.
		 */
		DUK_ASSERT(thr->callstack_top >= 1);
		act = thr->callstack + thr->callstack_top - 1;

		if (act->lex_env == NULL) {
			DUK_ASSERT(act->var_env == NULL);
			DUK_DDD(DUK_DDDPRINT("delayed environment initialization"));

			/* this may have side effects, so re-lookup act */
			duk_js_init_activation_environment_records_delayed(thr, act);
			act = thr->callstack + thr->callstack_top - 1;
		}
		DUK_ASSERT(act->lex_env != NULL);
		DUK_ASSERT(act->var_env != NULL);
		DUK_ASSERT(DUK_ACT_GET_FUNC(act) != NULL);
		DUK_UNREF(act);  /* unreferenced without assertions */

		act = thr->callstack + thr->callstack_top - 1;
		act_lex_env = act->lex_env;
		act = NULL;  /* invalidated */

		(void) duk_push_object_helper_proto(ctx,
		                                    DUK_HOBJECT_FLAG_EXTENSIBLE |
		                                    DUK_HOBJECT_CLASS_AS_FLAGS(DUK_HOBJECT_CLASS_DECENV),
		                                    act_lex_env);
		new_env = duk_require_hobject(ctx, -1);
		DUK_ASSERT(new_env != NULL);
		DUK_DDD(DUK_DDDPRINT("new_env allocated: %!iO", (duk_heaphdr *) new_env));

		/* Note: currently the catch binding is handled without a register
		 * binding because we don't support dynamic register bindings (they
		 * must be fixed for an entire function).  So, there is no need to
		 * record regbases etc.
		 */

		DUK_ASSERT(thr->catchstack[cat_idx].h_varname != NULL);
		duk_push_hstring(ctx, thr->catchstack[cat_idx].h_varname);
		duk_push_tval(ctx, &thr->heap->lj.value1);
		duk_xdef_prop(ctx, -3, DUK_PROPDESC_FLAGS_W);  /* writable, not configurable */

		act = thr->callstack + thr->callstack_top - 1;
		act->lex_env = new_env;
		DUK_HOBJECT_INCREF(thr, new_env);  /* reachable through activation */

		DUK_CAT_SET_LEXENV_ACTIVE(&thr->catchstack[cat_idx]);

		duk_pop(ctx);

		DUK_DDD(DUK_DDDPRINT("new_env finished: %!iO", (duk_heaphdr *) new_env));
	}

	if (is_finally) {
		DUK_CAT_CLEAR_FINALLY_ENABLED(&thr->catchstack[cat_idx]);
	} else {
		DUK_CAT_CLEAR_CATCH_ENABLED(&thr->catchstack[cat_idx]);
	}
}

DUK_LOCAL void duk__handle_label(duk_hthread *thr, duk_size_t cat_idx) {
	duk_activation *act;

	/* no callstack changes, no value stack changes */

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->callstack_top >= 1);

	act = thr->callstack + thr->callstack_top - 1;

	DUK_ASSERT(DUK_ACT_GET_FUNC(act) != NULL);
	DUK_ASSERT(DUK_HOBJECT_HAS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(act)));

	/* +0 = break, +1 = continue */
	act->pc = thr->catchstack[cat_idx].pc_base + (thr->heap->lj.type == DUK_LJ_TYPE_CONTINUE ? 1 : 0);
	act = NULL;  /* invalidated */

	duk_hthread_catchstack_unwind(thr, cat_idx + 1);  /* keep label catcher */
	/* no need to unwind callstack */

	/* valstack should not need changes */
#if defined(DUK_USE_ASSERTIONS)
	act = thr->callstack + thr->callstack_top - 1;
	DUK_ASSERT((duk_size_t) (thr->valstack_top - thr->valstack_bottom) ==
	           (duk_size_t) ((duk_hcompiledfunction *) DUK_ACT_GET_FUNC(act))->nregs);
#endif
}

/* Note: called for DUK_LJ_TYPE_YIELD and for DUK_LJ_TYPE_RETURN, when a
 * return terminates a thread and yields to the resumer.
 */
DUK_LOCAL void duk__handle_yield(duk_hthread *thr, duk_hthread *resumer, duk_size_t act_idx) {
	duk_tval tv_tmp;
	duk_tval *tv1;

	/* this may also be called for DUK_LJ_TYPE_RETURN; this is OK as long as
	 * lj.value1 is correct.
	 */

	DUK_ASSERT(DUK_ACT_GET_FUNC(resumer->callstack + act_idx) != NULL);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(resumer->callstack + act_idx)));  /* resume caller must be an ecmascript func */

	DUK_DDD(DUK_DDDPRINT("resume idx_retval is %ld", (long) resumer->callstack[act_idx].idx_retval));

	tv1 = resumer->valstack + resumer->callstack[act_idx].idx_retval;  /* return value from Duktape.Thread.resume() */
	DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
	DUK_TVAL_SET_TVAL(tv1, &thr->heap->lj.value1);
	DUK_TVAL_INCREF(thr, tv1);
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

	duk_hthread_callstack_unwind(resumer, act_idx + 1);  /* unwind to 'resume' caller */

	/* no need to unwind catchstack */
	duk__reconfig_valstack(resumer, act_idx, 1);  /* 1 = have retval */

	/* caller must change active thread, and set thr->resumer to NULL */
}

DUK_LOCAL
duk_small_uint_t duk__handle_longjmp(duk_hthread *thr,
                                     duk_hthread *entry_thread,
                                     duk_size_t entry_callstack_top) {
	duk_tval tv_tmp;
	duk_size_t entry_callstack_index;
	duk_small_uint_t retval = DUK__LONGJMP_RESTART;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(entry_thread != NULL);
	DUK_ASSERT(entry_callstack_top > 0);  /* guarantees entry_callstack_top - 1 >= 0 */

	entry_callstack_index = entry_callstack_top - 1;

	/* 'thr' is the current thread, as no-one resumes except us and we
	 * switch 'thr' in that case.
	 */

	/*
	 *  (Re)try handling the longjmp.
	 *
	 *  A longjmp handler may convert the longjmp to a different type and
	 *  "virtually" rethrow by goto'ing to 'check_longjmp'.  Before the goto,
	 *  the following must be updated:
	 *    - the heap 'lj' state
	 *    - 'thr' must reflect the "throwing" thread
	 */

 check_longjmp:

	DUK_DD(DUK_DDPRINT("handling longjmp: type=%ld, value1=%!T, value2=%!T, iserror=%ld",
	                   (long) thr->heap->lj.type,
	                   (duk_tval *) &thr->heap->lj.value1,
	                   (duk_tval *) &thr->heap->lj.value2,
	                   (long) thr->heap->lj.iserror));

	switch (thr->heap->lj.type) {

	case DUK_LJ_TYPE_RESUME: {
		/*
		 *  Note: lj.value1 is 'value', lj.value2 is 'resumee'.
		 *  This differs from YIELD.
		 */

		duk_tval *tv;
		duk_tval *tv2;
		duk_size_t act_idx;
		duk_hthread *resumee;

		/* duk_bi_duk_object_yield() and duk_bi_duk_object_resume() ensure all of these are met */

		DUK_ASSERT(thr->state == DUK_HTHREAD_STATE_RUNNING);                                                         /* unchanged by Duktape.Thread.resume() */
		DUK_ASSERT(thr->callstack_top >= 2);                                                                         /* Ecmascript activation + Duktape.Thread.resume() activation */
		DUK_ASSERT(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 1) != NULL &&
		           DUK_HOBJECT_IS_NATIVEFUNCTION(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 1)) &&
		           ((duk_hnativefunction *) DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 1))->func == duk_bi_thread_resume);
		DUK_ASSERT(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 2) != NULL &&
		           DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 2)));      /* an Ecmascript function */
		DUK_ASSERT_DISABLE((thr->callstack + thr->callstack_top - 2)->idx_retval >= 0);                              /* unsigned */

		tv = &thr->heap->lj.value2;  /* resumee */
		DUK_ASSERT(DUK_TVAL_IS_OBJECT(tv));
		DUK_ASSERT(DUK_TVAL_GET_OBJECT(tv) != NULL);
		DUK_ASSERT(DUK_HOBJECT_IS_THREAD(DUK_TVAL_GET_OBJECT(tv)));
		resumee = (duk_hthread *) DUK_TVAL_GET_OBJECT(tv);

		DUK_ASSERT(resumee != NULL);
		DUK_ASSERT(resumee->resumer == NULL);
		DUK_ASSERT(resumee->state == DUK_HTHREAD_STATE_INACTIVE ||
		           resumee->state == DUK_HTHREAD_STATE_YIELDED);                                                     /* checked by Duktape.Thread.resume() */
		DUK_ASSERT(resumee->state != DUK_HTHREAD_STATE_YIELDED ||
		           resumee->callstack_top >= 2);                                                                     /* YIELDED: Ecmascript activation + Duktape.Thread.yield() activation */
		DUK_ASSERT(resumee->state != DUK_HTHREAD_STATE_YIELDED ||
		           (DUK_ACT_GET_FUNC(resumee->callstack + resumee->callstack_top - 1) != NULL &&
		            DUK_HOBJECT_IS_NATIVEFUNCTION(DUK_ACT_GET_FUNC(resumee->callstack + resumee->callstack_top - 1)) &&
		            ((duk_hnativefunction *) DUK_ACT_GET_FUNC(resumee->callstack + resumee->callstack_top - 1))->func == duk_bi_thread_yield));
		DUK_ASSERT(resumee->state != DUK_HTHREAD_STATE_YIELDED ||
		           (DUK_ACT_GET_FUNC(resumee->callstack + resumee->callstack_top - 2) != NULL &&
		            DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(resumee->callstack + resumee->callstack_top - 2))));      /* an Ecmascript function */
		DUK_ASSERT_DISABLE(resumee->state != DUK_HTHREAD_STATE_YIELDED ||
		           (resumee->callstack + resumee->callstack_top - 2)->idx_retval >= 0);                              /* idx_retval unsigned */
		DUK_ASSERT(resumee->state != DUK_HTHREAD_STATE_INACTIVE ||
		           resumee->callstack_top == 0);                                                                     /* INACTIVE: no activation, single function value on valstack */
		DUK_ASSERT(resumee->state != DUK_HTHREAD_STATE_INACTIVE ||
		           (resumee->valstack_top == resumee->valstack + 1 &&
		            DUK_TVAL_IS_OBJECT(resumee->valstack_top - 1) &&
		            DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_TVAL_GET_OBJECT(resumee->valstack_top - 1))));

		if (thr->heap->lj.iserror) {
			/*
			 *  Throw the error in the resumed thread's context; the
			 *  error value is pushed onto the resumee valstack.
			 *
			 *  Note: the callstack of the target may empty in this case
			 *  too (i.e. the target thread has never been resumed).  The
			 *  value stack will contain the initial function in that case,
			 *  which we simply ignore.
			 */

			resumee->resumer = thr;
			resumee->state = DUK_HTHREAD_STATE_RUNNING;
			thr->state = DUK_HTHREAD_STATE_RESUMED;
			DUK_HEAP_SWITCH_THREAD(thr->heap, resumee);
			thr = resumee;

			thr->heap->lj.type = DUK_LJ_TYPE_THROW;

			/* thr->heap->lj.value1 is already the value to throw */
			/* thr->heap->lj.value2 is 'thread', will be wiped out at the end */

			DUK_ASSERT(thr->heap->lj.iserror);  /* already set */

			DUK_DD(DUK_DDPRINT("-> resume with an error, converted to a throw in the resumee, propagate"));
			goto check_longjmp;
		} else if (resumee->state == DUK_HTHREAD_STATE_YIELDED) {
			act_idx = resumee->callstack_top - 2;  /* Ecmascript function */
			DUK_ASSERT_DISABLE(resumee->callstack[act_idx].idx_retval >= 0);  /* unsigned */

			tv = resumee->valstack + resumee->callstack[act_idx].idx_retval;  /* return value from Duktape.Thread.yield() */
			DUK_ASSERT(tv >= resumee->valstack && tv < resumee->valstack_top);
			tv2 = &thr->heap->lj.value1;
			DUK_TVAL_SET_TVAL(&tv_tmp, tv);
			DUK_TVAL_SET_TVAL(tv, tv2);
			DUK_TVAL_INCREF(thr, tv);
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

			duk_hthread_callstack_unwind(resumee, act_idx + 1);  /* unwind to 'yield' caller */

			/* no need to unwind catchstack */

			duk__reconfig_valstack(resumee, act_idx, 1);  /* 1 = have retval */

			resumee->resumer = thr;
			resumee->state = DUK_HTHREAD_STATE_RUNNING;
			thr->state = DUK_HTHREAD_STATE_RESUMED;
			DUK_HEAP_SWITCH_THREAD(thr->heap, resumee);
#if 0
			thr = resumee;  /* not needed, as we exit right away */
#endif
			DUK_DD(DUK_DDPRINT("-> resume with a value, restart execution in resumee"));
			retval = DUK__LONGJMP_RESTART;
			goto wipe_and_return;
		} else {
			duk_small_uint_t call_flags;
			duk_bool_t setup_rc;

			/* resumee: [... initial_func]  (currently actually: [initial_func]) */

			duk_push_undefined((duk_context *) resumee);
			tv = &thr->heap->lj.value1;
			duk_push_tval((duk_context *) resumee, tv);

			/* resumee: [... initial_func undefined(= this) resume_value ] */

			call_flags = DUK_CALL_FLAG_IS_RESUME;  /* is resume, not a tailcall */

			setup_rc = duk_handle_ecma_call_setup(resumee,
			                                      1,              /* num_stack_args */
			                                      call_flags);    /* call_flags */
			if (setup_rc == 0) {
				/* Shouldn't happen but check anyway. */
				DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, DUK_STR_INTERNAL_ERROR);
			}

			resumee->resumer = thr;
			resumee->state = DUK_HTHREAD_STATE_RUNNING;
			thr->state = DUK_HTHREAD_STATE_RESUMED;
			DUK_HEAP_SWITCH_THREAD(thr->heap, resumee);
#if 0
			thr = resumee;  /* not needed, as we exit right away */
#endif
			DUK_DD(DUK_DDPRINT("-> resume with a value, restart execution in resumee"));
			retval = DUK__LONGJMP_RESTART;
			goto wipe_and_return;
		}
		DUK_UNREACHABLE();
		break;  /* never here */
	}

	case DUK_LJ_TYPE_YIELD: {
		/*
		 *  Currently only allowed only if yielding thread has only
		 *  Ecmascript activations (except for the Duktape.Thread.yield()
		 *  call at the callstack top) and none of them constructor
		 *  calls.
		 *
		 *  This excludes the 'entry' thread which will always have
		 *  a preventcount > 0.
		 */

		duk_hthread *resumer;

		/* duk_bi_duk_object_yield() and duk_bi_duk_object_resume() ensure all of these are met */

		DUK_ASSERT(thr != entry_thread);                                                                             /* Duktape.Thread.yield() should prevent */
		DUK_ASSERT(thr->state == DUK_HTHREAD_STATE_RUNNING);                                                         /* unchanged from Duktape.Thread.yield() */
		DUK_ASSERT(thr->callstack_top >= 2);                                                                         /* Ecmascript activation + Duktape.Thread.yield() activation */
		DUK_ASSERT(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 1) != NULL &&
		           DUK_HOBJECT_IS_NATIVEFUNCTION(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 1)) &&
		           ((duk_hnativefunction *) DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 1))->func == duk_bi_thread_yield);
		DUK_ASSERT(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 2) != NULL &&
		           DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 2)));      /* an Ecmascript function */
		DUK_ASSERT_DISABLE((thr->callstack + thr->callstack_top - 2)->idx_retval >= 0);                              /* unsigned */

		resumer = thr->resumer;

		DUK_ASSERT(resumer != NULL);
		DUK_ASSERT(resumer->state == DUK_HTHREAD_STATE_RESUMED);                                                     /* written by a previous RESUME handling */
		DUK_ASSERT(resumer->callstack_top >= 2);                                                                     /* Ecmascript activation + Duktape.Thread.resume() activation */
		DUK_ASSERT(DUK_ACT_GET_FUNC(resumer->callstack + resumer->callstack_top - 1) != NULL &&
		           DUK_HOBJECT_IS_NATIVEFUNCTION(DUK_ACT_GET_FUNC(resumer->callstack + resumer->callstack_top - 1)) &&
		           ((duk_hnativefunction *) DUK_ACT_GET_FUNC(resumer->callstack + resumer->callstack_top - 1))->func == duk_bi_thread_resume);
		DUK_ASSERT(DUK_ACT_GET_FUNC(resumer->callstack + resumer->callstack_top - 2) != NULL &&
		           DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(resumer->callstack + resumer->callstack_top - 2)));        /* an Ecmascript function */
		DUK_ASSERT_DISABLE((resumer->callstack + resumer->callstack_top - 2)->idx_retval >= 0);                      /* unsigned */

		if (thr->heap->lj.iserror) {
			thr->state = DUK_HTHREAD_STATE_YIELDED;
			thr->resumer = NULL;
			resumer->state = DUK_HTHREAD_STATE_RUNNING;
			DUK_HEAP_SWITCH_THREAD(thr->heap, resumer);
			thr = resumer;

			thr->heap->lj.type = DUK_LJ_TYPE_THROW;
			/* lj.value1 is already set */
			DUK_ASSERT(thr->heap->lj.iserror);  /* already set */

			DUK_DD(DUK_DDPRINT("-> yield an error, converted to a throw in the resumer, propagate"));
			goto check_longjmp;
		} else {
			duk__handle_yield(thr, resumer, resumer->callstack_top - 2);

			thr->state = DUK_HTHREAD_STATE_YIELDED;
			thr->resumer = NULL;
			resumer->state = DUK_HTHREAD_STATE_RUNNING;
			DUK_HEAP_SWITCH_THREAD(thr->heap, resumer);
#if 0
			thr = resumer;  /* not needed, as we exit right away */
#endif

			DUK_DD(DUK_DDPRINT("-> yield a value, restart execution in resumer"));
			retval = DUK__LONGJMP_RESTART;
			goto wipe_and_return;
		}
		DUK_UNREACHABLE();
		break;  /* never here */
	}

	case DUK_LJ_TYPE_RETURN: {
		/*
		 *  Four possible outcomes:
		 *    * A 'finally' in the same function catches the 'return'.
		 *      (or)
		 *    * The return happens at the entry level of the bytecode
		 *      executor, so return from the executor (in C stack).
		 *      (or)
		 *    * There is a calling (Ecmascript) activation in the call
		 *      stack => return to it.
		 *      (or)
		 *    * There is no calling activation, and the thread is
		 *      terminated.  There is always a resumer in this case,
		 *      which gets the return value similarly to a 'yield'
		 *      (except that the current thread can no longer be
		 *      resumed).
		 */

		duk_tval *tv1;
		duk_hthread *resumer;
		duk_catcher *cat;
		duk_size_t orig_callstack_index;

		DUK_ASSERT(thr != NULL);
		DUK_ASSERT(thr->callstack_top >= 1);
		DUK_ASSERT(thr->catchstack != NULL);

		/* XXX: does not work if thr->catchstack is NULL */
		/* XXX: does not work if thr->catchstack is allocated but lowest pointer */

		cat = thr->catchstack + thr->catchstack_top - 1;  /* may be < thr->catchstack initially */
		DUK_ASSERT(thr->callstack_top > 0);  /* ensures callstack_top - 1 >= 0 */
		orig_callstack_index = thr->callstack_top - 1;

		while (cat >= thr->catchstack) {
			if (cat->callstack_index != orig_callstack_index) {
				break;
			}
			if (DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_TCF &&
			    DUK_CAT_HAS_FINALLY_ENABLED(cat)) {
				/* 'finally' catches */
				duk__handle_catch_or_finally(thr,
				                             cat - thr->catchstack,
				                             1); /* is_finally */

				DUK_DD(DUK_DDPRINT("-> return caught by a finally (in the same function), restart execution"));
				retval = DUK__LONGJMP_RESTART;
				goto wipe_and_return;
			}
			cat--;
		}
		/* if out of catchstack, cat = thr->catchstack - 1 */

		DUK_DD(DUK_DDPRINT("no catcher in catch stack, return to calling activation / yield"));

		/* return to calling activation (if any) */

		if (thr == entry_thread &&
		    thr->callstack_top == entry_callstack_top) {
			/* return to the bytecode executor caller */

			duk_push_tval((duk_context *) thr, &thr->heap->lj.value1);

			/* [ ... retval ] */

			DUK_DD(DUK_DDPRINT("-> return propagated up to entry level, exit bytecode executor"));
			retval = DUK__LONGJMP_FINISHED;
			goto wipe_and_return;
		}

		if (thr->callstack_top >= 2) {
			/* there is a caller; it MUST be an Ecmascript caller (otherwise it would
			 * match entry level check)
			 */

			DUK_DDD(DUK_DDDPRINT("slow return to Ecmascript caller, idx_retval=%ld, lj_value1=%!T",
			                     (long) (thr->callstack + thr->callstack_top - 2)->idx_retval,
			                     (duk_tval *) &thr->heap->lj.value1));

			DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 2)));   /* must be ecmascript */

			tv1 = thr->valstack + (thr->callstack + thr->callstack_top - 2)->idx_retval;
			DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
			DUK_TVAL_SET_TVAL(tv1, &thr->heap->lj.value1);
			DUK_TVAL_INCREF(thr, tv1);
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

			DUK_DDD(DUK_DDDPRINT("return value at idx_retval=%ld is %!T",
			                     (long) (thr->callstack + thr->callstack_top - 2)->idx_retval,
			                     (duk_tval *) (thr->valstack + (thr->callstack + thr->callstack_top - 2)->idx_retval)));

			duk_hthread_catchstack_unwind(thr, (cat - thr->catchstack) + 1);  /* leave 'cat' as top catcher (also works if catchstack exhausted) */
			duk_hthread_callstack_unwind(thr, thr->callstack_top - 1);
			duk__reconfig_valstack(thr, thr->callstack_top - 1, 1);    /* new top, i.e. callee */

			DUK_DD(DUK_DDPRINT("-> return not caught, restart execution in caller"));
			retval = DUK__LONGJMP_RESTART;
			goto wipe_and_return;
		}

		DUK_DD(DUK_DDPRINT("no calling activation, thread finishes (similar to yield)"));

		DUK_ASSERT(thr->resumer != NULL);
		DUK_ASSERT(thr->resumer->callstack_top >= 2);  /* Ecmascript activation + Duktape.Thread.resume() activation */
		DUK_ASSERT(DUK_ACT_GET_FUNC(thr->resumer->callstack + thr->resumer->callstack_top - 1) != NULL &&
		           DUK_HOBJECT_IS_NATIVEFUNCTION(DUK_ACT_GET_FUNC(thr->resumer->callstack + thr->resumer->callstack_top - 1)) &&
		           ((duk_hnativefunction *) DUK_ACT_GET_FUNC(thr->resumer->callstack + thr->resumer->callstack_top - 1))->func == duk_bi_thread_resume);  /* Duktape.Thread.resume() */
		DUK_ASSERT(DUK_ACT_GET_FUNC(thr->resumer->callstack + thr->resumer->callstack_top - 2) != NULL &&
		           DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(thr->resumer->callstack + thr->resumer->callstack_top - 2)));  /* an Ecmascript function */
		DUK_ASSERT_DISABLE((thr->resumer->callstack + thr->resumer->callstack_top - 2)->idx_retval >= 0);                /* unsigned */
		DUK_ASSERT(thr->state == DUK_HTHREAD_STATE_RUNNING);
		DUK_ASSERT(thr->resumer->state == DUK_HTHREAD_STATE_RESUMED);

		resumer = thr->resumer;

		duk__handle_yield(thr, resumer, resumer->callstack_top - 2);

		duk_hthread_terminate(thr);  /* updates thread state, minimizes its allocations */
		DUK_ASSERT(thr->state == DUK_HTHREAD_STATE_TERMINATED);

		thr->resumer = NULL;
		resumer->state = DUK_HTHREAD_STATE_RUNNING;
		DUK_HEAP_SWITCH_THREAD(thr->heap, resumer);
#if 0
		thr = resumer;  /* not needed */
#endif

		DUK_DD(DUK_DDPRINT("-> return not caught, thread terminated; handle like yield, restart execution in resumer"));
		retval = DUK__LONGJMP_RESTART;
		goto wipe_and_return;
	}

	case DUK_LJ_TYPE_BREAK:
	case DUK_LJ_TYPE_CONTINUE: {
		/*
		 *  Find a matching label catcher or 'finally' catcher in
		 *  the same function.
		 *
		 *  A label catcher must always exist and will match unless
		 *  a 'finally' captures the break/continue first.  It is the
		 *  compiler's responsibility to ensure that labels are used
		 *  correctly.
		 */

		duk_catcher *cat;
		duk_size_t orig_callstack_index;
		duk_uint_t lj_label;

		cat = thr->catchstack + thr->catchstack_top - 1;
		orig_callstack_index = cat->callstack_index;

		DUK_ASSERT(DUK_TVAL_IS_NUMBER(&thr->heap->lj.value1));
		lj_label = (duk_uint_t) DUK_TVAL_GET_NUMBER(&thr->heap->lj.value1);

		DUK_DDD(DUK_DDDPRINT("handling break/continue with label=%ld, callstack index=%ld",
		                     (long) lj_label, (long) cat->callstack_index));

		while (cat >= thr->catchstack) {
			if (cat->callstack_index != orig_callstack_index) {
				break;
			}
			DUK_DDD(DUK_DDDPRINT("considering catcher %ld: type=%ld label=%ld",
			                     (long) (cat - thr->catchstack),
			                     (long) DUK_CAT_GET_TYPE(cat),
			                     (long) DUK_CAT_GET_LABEL(cat)));

			if (DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_TCF &&
			    DUK_CAT_HAS_FINALLY_ENABLED(cat)) {
				/* finally catches */
				duk__handle_catch_or_finally(thr,
				                             cat - thr->catchstack,
				                             1); /* is_finally */

				DUK_DD(DUK_DDPRINT("-> break/continue caught by a finally (in the same function), restart execution"));
				retval = DUK__LONGJMP_RESTART;
				goto wipe_and_return;
			}
			if (DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_LABEL &&
			    (duk_uint_t) DUK_CAT_GET_LABEL(cat) == lj_label) {
				/* found label */
				duk__handle_label(thr,
				                  cat - thr->catchstack);

				DUK_DD(DUK_DDPRINT("-> break/continue caught by a label catcher (in the same function), restart execution"));
				retval = DUK__LONGJMP_RESTART;
				goto wipe_and_return;
			}
			cat--;
		}

		/* should never happen, but be robust */
		DUK_D(DUK_DPRINT("break/continue not caught by anything in the current function (should never happen)"));
		goto convert_to_internal_error;
	}

	case DUK_LJ_TYPE_THROW: {
		/*
		 *  Three possible outcomes:
		 *    * A try or finally catcher is found => resume there.
		 *      (or)
		 *    * The error propagates to the bytecode executor entry
		 *      level (and we're in the entry thread) => rethrow
		 *      with a new longjmp(), after restoring the previous
		 *      catchpoint.
		 *    * The error is not caught in the current thread, so
		 *      the thread finishes with an error.  This works like
		 *      a yielded error, except that the thread is finished
		 *      and can no longer be resumed.  (There is always a
		 *      resumer in this case.)
		 *
		 *  Note: until we hit the entry level, there can only be
		 *  Ecmascript activations.
		 */

		duk_catcher *cat;
		duk_hthread *resumer;

		cat = thr->catchstack + thr->catchstack_top - 1;
		while (cat >= thr->catchstack) {
			if (thr == entry_thread &&
			    cat->callstack_index < entry_callstack_index) {
				/* entry level reached */
				break;
			}

			if (DUK_CAT_HAS_CATCH_ENABLED(cat)) {
				/* try catches */
				DUK_ASSERT(DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_TCF);

				duk__handle_catch_or_finally(thr,
				                             cat - thr->catchstack,
				                             0); /* is_finally */

				DUK_DD(DUK_DDPRINT("-> throw caught by a 'catch' clause, restart execution"));
				retval = DUK__LONGJMP_RESTART;
				goto wipe_and_return;
			}

			if (DUK_CAT_HAS_FINALLY_ENABLED(cat)) {
				DUK_ASSERT(DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_TCF);
				DUK_ASSERT(!DUK_CAT_HAS_CATCH_ENABLED(cat));

				duk__handle_catch_or_finally(thr,
				                             cat - thr->catchstack,
				                             1); /* is_finally */

				DUK_DD(DUK_DDPRINT("-> throw caught by a 'finally' clause, restart execution"));
				retval = DUK__LONGJMP_RESTART;
				goto wipe_and_return;
			}

			cat--;
		}

		if (thr == entry_thread) {
			/* not caught by anything before entry level; rethrow and let the
			 * final catcher unwind everything
			 */
#if 0
			duk_hthread_catchstack_unwind(thr, (cat - thr->catchstack) + 1);  /* leave 'cat' as top catcher (also works if catchstack exhausted) */
			duk_hthread_callstack_unwind(thr, entry_callstack_index + 1);

#endif
			DUK_D(DUK_DPRINT("-> throw propagated up to entry level, rethrow and exit bytecode executor"));
			retval = DUK__LONGJMP_RETHROW;
			goto just_return;
			/* Note: MUST NOT wipe_and_return here, as heap->lj must remain intact */
		}

		DUK_DD(DUK_DDPRINT("not caught by current thread, yield error to resumer"));

		/* not caught by current thread, thread terminates (yield error to resumer);
		 * note that this may cause a cascade if the resumer terminates with an uncaught
		 * exception etc (this is OK, but needs careful testing)
		 */

		DUK_ASSERT(thr->resumer != NULL);
		DUK_ASSERT(thr->resumer->callstack_top >= 2);  /* Ecmascript activation + Duktape.Thread.resume() activation */
		DUK_ASSERT(DUK_ACT_GET_FUNC(thr->resumer->callstack + thr->resumer->callstack_top - 1) != NULL &&
		           DUK_HOBJECT_IS_NATIVEFUNCTION(DUK_ACT_GET_FUNC(thr->resumer->callstack + thr->resumer->callstack_top - 1)) &&
		           ((duk_hnativefunction *) DUK_ACT_GET_FUNC(thr->resumer->callstack + thr->resumer->callstack_top - 1))->func == duk_bi_thread_resume);  /* Duktape.Thread.resume() */
		DUK_ASSERT(DUK_ACT_GET_FUNC(thr->resumer->callstack + thr->resumer->callstack_top - 2) != NULL &&
		           DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(thr->resumer->callstack + thr->resumer->callstack_top - 2)));  /* an Ecmascript function */

		resumer = thr->resumer;

		/* reset longjmp */

		DUK_ASSERT(thr->heap->lj.type == DUK_LJ_TYPE_THROW);  /* already set */
		/* lj.value1 already set */

		duk_hthread_terminate(thr);  /* updates thread state, minimizes its allocations */
		DUK_ASSERT(thr->state == DUK_HTHREAD_STATE_TERMINATED);

		thr->resumer = NULL;
		resumer->state = DUK_HTHREAD_STATE_RUNNING;
		DUK_HEAP_SWITCH_THREAD(thr->heap, resumer);
		thr = resumer;
		goto check_longjmp;
	}

	case DUK_LJ_TYPE_NORMAL: {
		DUK_D(DUK_DPRINT("caught DUK_LJ_TYPE_NORMAL, should never happen, treat as internal error"));
		goto convert_to_internal_error;
	}

	default: {
		/* should never happen, but be robust */
		DUK_D(DUK_DPRINT("caught unknown longjmp type %ld, treat as internal error", (long) thr->heap->lj.type));
		goto convert_to_internal_error;
	}

	}  /* end switch */

	DUK_UNREACHABLE();

 wipe_and_return:
	/* this is not strictly necessary, but helps debugging */
	thr->heap->lj.type = DUK_LJ_TYPE_UNKNOWN;
	thr->heap->lj.iserror = 0;

	DUK_TVAL_SET_TVAL(&tv_tmp, &thr->heap->lj.value1);
	DUK_TVAL_SET_UNDEFINED_UNUSED(&thr->heap->lj.value1);
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

	DUK_TVAL_SET_TVAL(&tv_tmp, &thr->heap->lj.value2);
	DUK_TVAL_SET_UNDEFINED_UNUSED(&thr->heap->lj.value2);
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

 just_return:
	return retval;

 convert_to_internal_error:
	/* This could also be thrown internally (set the error, goto check_longjmp),
	 * but it's better for internal errors to bubble outwards.
	 */
	DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, DUK_STR_INTERNAL_ERROR_EXEC_LONGJMP);
	DUK_UNREACHABLE();
	return retval;
}

/* XXX: Disabled for 1.0 release.  This needs to handle unwinding for label
 * sites (which are created for explicit labels but also for control statements
 * like for-loops).  At that point it's quite close to the "slow return" handler
 * except for longjmp().  Perhaps all returns could initially be handled as fast
 * returns and only converted to longjmp()s when basic handling won't do?
 */
#if 0
/* Try a fast return.  Return false if fails, so that a slow return can be done
 * instead.
 */
DUK_LOCAL
duk_bool_t duk__handle_fast_return(duk_hthread *thr,
                                   duk_tval *tv_retval,
                                   duk_hthread *entry_thread,
                                   duk_size_t entry_callstack_top) {
	duk_tval tv_tmp;
	duk_tval *tv1;

	/* retval == NULL indicates 'undefined' return value */

	if (thr == entry_thread && thr->callstack_top == entry_callstack_top) {
		DUK_DDD(DUK_DDDPRINT("reject fast return: return would exit bytecode executor to caller"));
		return 0;
	}
	if (thr->callstack_top <= 1) {
		DUK_DDD(DUK_DDDPRINT("reject fast return: there is no caller in this callstack (thread yield)"));
		return 0;
	}

	/* There is a caller, and it must be an Ecmascript caller (otherwise
	 * it would have matched the entry level check).
	 */
	DUK_ASSERT(thr->callstack_top >= 2);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 2)));   /* must be ecmascript */

	tv1 = thr->valstack + (thr->callstack + thr->callstack_top - 2)->idx_retval;
	DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
	if (tv_retval) {
		DUK_TVAL_SET_TVAL(tv1, tv_retval);
		DUK_TVAL_INCREF(thr, tv1);
	} else {
		DUK_TVAL_SET_UNDEFINED_ACTUAL(tv1);
		/* no need to incref */
	}
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

	/* No catchstack to unwind. */
#if 0
	duk_hthread_catchstack_unwind(thr, (cat - thr->catchstack) + 1);  /* leave 'cat' as top catcher (also works if catchstack exhausted) */
#endif
	duk_hthread_callstack_unwind(thr, thr->callstack_top - 1);
	duk__reconfig_valstack(thr, thr->callstack_top - 1, 1);    /* new top, i.e. callee */

	DUK_DDD(DUK_DDDPRINT("fast return accepted"));
	return 1;
}
#endif

/*
 *  Executor interrupt handling
 *
 *  The handler is called whenever the interrupt countdown reaches zero
 *  (or below).  The handler must perform whatever checks are activated,
 *  e.g. check for cumulative step count to impose an execution step
 *  limit or check for breakpoints or other debugger interaction.
 *
 *  When the actions are done, the handler must reinit the interrupt
 *  init and counter values.  The 'init' value must indicate how many
 *  bytecode instructions are executed before the next interrupt.  The
 *  counter must interface with the bytecode executor loop.  Concretely,
 *  the new init value is normally one higher than the new counter value.
 *  For instance, to execute exactly one bytecode instruction the init
 *  value is set to 1 and the counter to 0.  If an error is thrown by the
 *  interrupt handler, the counters are set to the same value (e.g. both
 *  to 0 to cause an interrupt when the next bytecode instruction is about
 *  to be executed after error handling).
 *
 *  Maintaining the init/counter value properly is important for accurate
 *  behavior.  For instance, executor step limit needs a cumulative step
 *  count which is simply computed as a sum of 'init' values.  This must
 *  work accurately even when single stepping.
 */

#if defined(DUK_USE_INTERRUPT_COUNTER)

#define DUK__INT_NOACTION    0    /* no specific action, resume normal execution */
#define DUK__INT_RESTART     1    /* must "goto restart_execution", e.g. breakpoints changed */

#if defined(DUK_USE_DEBUGGER_SUPPORT)
DUK_LOCAL void duk__interrupt_handle_debugger(duk_hthread *thr, duk_bool_t *out_immediate, duk_small_uint_t *out_interrupt_retval) {
	duk_context *ctx;
	duk_activation *act;
	duk_breakpoint *bp;
	duk_breakpoint **bp_active;
	duk_uint_fast32_t line = 0;
	duk_bool_t send_status;
	duk_bool_t process_messages;
	duk_bool_t processed_messages = 0;

	ctx = (duk_context *) thr;
	act = thr->callstack + thr->callstack_top - 1;

	/* It might seem that replacing 'thr->heap' with just 'heap' below
	 * might be a good idea, but it increases code size slightly
	 * (probably due to unnecessary spilling) at least on x64.
	 */

	/*
	 *  Breakpoint and step state checks
	 */

	if (act->flags & DUK_ACT_FLAG_BREAKPOINT_ACTIVE ||
	    (thr->heap->dbg_step_thread == thr &&
	     thr->heap->dbg_step_csindex == thr->callstack_top - 1)) {
		line = duk_debug_curr_line(thr);

		if (act->prev_line != line) {
			DUK_DDD(DUK_DDDPRINT("PC=%ld line=%ld; line transition: %ld -> %ld",
			                     (long) act->pc, (long) line, (long) act->prev_line, (long) line));

			/* Stepped?  Step out is handled by callstack unwind. */
			if ((thr->heap->dbg_step_type == DUK_STEP_TYPE_INTO ||
			     thr->heap->dbg_step_type == DUK_STEP_TYPE_OVER) &&
			    (thr->heap->dbg_step_thread == thr) &&
			    (thr->heap->dbg_step_csindex == thr->callstack_top - 1) &&
			    (line != thr->heap->dbg_step_startline)) {
				DUK_D(DUK_DPRINT("STEP STATE TRIGGERED PAUSE at line %ld",
				                 (long) line));

				DUK_HEAP_SET_PAUSED(thr->heap);
			}

			/* Check for breakpoints only on line transition.
			 * Breakpoint is triggered when we enter the target
			 * line from a different line, and the previous line
			 * was within the same function.
			 *
			 * This condition is tricky: the condition used to be
			 * that transition to -or across- the breakpoint line
			 * triggered the breakpoint.  This seems intuitively
			 * better because it handles breakpoints on lines with
			 * no emitted opcodes; but this leads to the issue
			 * described in: https://github.com/svaarala/duktape/issues/263.
			 */
			bp_active = thr->heap->dbg_breakpoints_active;
			for (;;) {
				bp = *bp_active++;
				if (bp == NULL) {
					break;
				}
				DUK_ASSERT(bp->filename != NULL);
				if (act->prev_line != bp->line && line == bp->line) {
					DUK_D(DUK_DPRINT("BREAKPOINT TRIGGERED at %!O:%ld",
					                 (duk_heaphdr *) bp->filename, (long) bp->line));

					DUK_HEAP_SET_PAUSED(thr->heap);
				}
			}
		} else {
			DUK_DDD(DUK_DDDPRINT("PC=%ld line=%ld", (long) act->pc, (long) line));
		}

		act->prev_line = line;
	}

	/*
	 *  Rate limit check for sending status update or peeking into
	 *  the debug transport.  Both can be expensive operations that
	 *  we don't want to do on every opcode.
	 *
	 *  Making sure the interval remains reasonable on a wide variety
	 *  of targets and bytecode is difficult without a timestamp, so
	 *  we use a Date-provided timestamp for the rate limit check.
	 *  But since it's also expensive to get a timestamp, a bytecode
	 *  counter is used to rate limit getting timestamps.
	 */

	if (thr->heap->dbg_state_dirty || thr->heap->dbg_paused) {
		send_status = 1;
	} else {
		send_status = 0;
	}

	if (thr->heap->dbg_paused) {
		process_messages = 1;
	} else {
		process_messages = 0;
	}

	/* XXX: remove heap->dbg_exec_counter, use heap->inst_count_interrupt instead? */
	thr->heap->dbg_exec_counter += thr->interrupt_init;
	if (thr->heap->dbg_exec_counter - thr->heap->dbg_last_counter >= DUK_HEAP_DBG_RATELIMIT_OPCODES) {
		/* Overflow of the execution counter is fine and doesn't break
		 * anything here.
		 */

		duk_double_t now, diff_last;

		thr->heap->dbg_last_counter = thr->heap->dbg_exec_counter;
		now = DUK_USE_DATE_GET_NOW(ctx);

		diff_last = now - thr->heap->dbg_last_time;
		if (diff_last < 0.0 || diff_last >= (duk_double_t) DUK_HEAP_DBG_RATELIMIT_MILLISECS) {
			/* Negative value checked so that a "time jump" works
			 * reasonably.
			 *
			 * Same interval is now used for status sending and
			 * peeking.
			 */

			thr->heap->dbg_last_time = now;
			send_status = 1;
			process_messages = 1;
		}
	}

	/*
	 *  Send status
	 */

	act = NULL;  /* may be changed */
	if (send_status) {
		duk_debug_send_status(thr);
		thr->heap->dbg_state_dirty = 0;
	}

	/*
	 *  Process messages.  If we're paused, we'll block for new messages.
	 *  if we're not paused, we'll process anything we can peek but won't
	 *  block for more.
	 */

	if (process_messages) {
		processed_messages = duk_debug_process_messages(thr, 0 /*no_block*/);
	}

	/* XXX: any case here where we need to re-send status? */

	/* Continue checked execution if there are breakpoints or we're stepping.
	 * Also use checked execution if paused flag is active - it shouldn't be
	 * because the debug message loop shouldn't terminate if it was.  Step out
	 * is handled by callstack unwind and doesn't need checked execution.
	 * Note that debugger may have detached due to error or explicit request
	 * above, so we must recheck attach status.
	 */

	if (DUK_HEAP_IS_DEBUGGER_ATTACHED(thr->heap)) {
		act = thr->callstack + thr->callstack_top - 1;  /* relookup, may have changed */
		if (act->flags & DUK_ACT_FLAG_BREAKPOINT_ACTIVE ||
		    ((thr->heap->dbg_step_type == DUK_STEP_TYPE_INTO ||
		      thr->heap->dbg_step_type == DUK_STEP_TYPE_OVER) &&
		     thr->heap->dbg_step_thread == thr &&
		     thr->heap->dbg_step_csindex == thr->callstack_top - 1) ||
		     thr->heap->dbg_paused) {
			*out_immediate = 1;
		}

		/* If we processed any debug messages breakpoints may have
		 * changed; restart execution to re-check active breakpoints.
		 */
		if (processed_messages) {
			DUK_D(DUK_DPRINT("processed debug messages, restart execution to recheck possibly changed breakpoints"));
			*out_interrupt_retval = DUK__INT_RESTART;
		}
	} else {
		DUK_D(DUK_DPRINT("debugger became detached, resume normal execution"));
	}
}
#endif  /* DUK_USE_DEBUGGER_SUPPORT */

DUK_LOCAL duk_small_uint_t duk__executor_interrupt(duk_hthread *thr) {
	duk_int_t ctr;
	duk_activation *act;
	duk_hcompiledfunction *fun;
	duk_bool_t immediate = 0;
	duk_small_uint_t retval;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(thr->callstack != NULL);
	DUK_ASSERT(thr->callstack_top > 0);

#if defined(DUK_USE_DEBUG)
	thr->heap->inst_count_interrupt += thr->interrupt_init;
	DUK_DD(DUK_DDPRINT("execution interrupt, counter=%ld, init=%ld, "
	                   "instruction counts: executor=%ld, interrupt=%ld",
	                   (long) thr->interrupt_counter, (long) thr->interrupt_init,
	                   (long) thr->heap->inst_count_exec, (long) thr->heap->inst_count_interrupt));
#endif

	retval = DUK__INT_NOACTION;
	ctr = DUK_HTHREAD_INTCTR_DEFAULT;

	/*
	 *  Avoid nested calls.  Concretely this happens during debugging, e.g.
	 *  when we eval() an expression.
	 */

	if (DUK_HEAP_HAS_INTERRUPT_RUNNING(thr->heap)) {
		DUK_DD(DUK_DDPRINT("nested executor interrupt, ignoring"));

		/* Set a high interrupt counter; the original executor
		 * interrupt invocation will rewrite before exiting.
		 */
		thr->interrupt_init = ctr;
		thr->interrupt_counter = ctr - 1;
		return DUK__INT_NOACTION;
	}
	DUK_HEAP_SET_INTERRUPT_RUNNING(thr->heap);

	act = thr->callstack + thr->callstack_top - 1;
	fun = (duk_hcompiledfunction *) DUK_ACT_GET_FUNC(act);
	DUK_ASSERT(DUK_HOBJECT_HAS_COMPILEDFUNCTION((duk_hobject *) fun));
	DUK_UNREF(fun);

#if defined(DUK_USE_EXEC_TIMEOUT_CHECK)
	/*
	 *  Execution timeout check
	 */

	if (DUK_USE_EXEC_TIMEOUT_CHECK(thr->heap->heap_udata)) {
		/* Keep throwing an error whenever we get here.  The unusual values
		 * are set this way because no instruction is ever executed, we just
		 * throw an error until all try/catch/finally and other catchpoints
		 * have been exhausted.  Duktape/C code gets control at each protected
		 * call but whenever it enters back into Duktape the RangeError gets
		 * raised.  User exec timeout check must consistently indicate a timeout
		 * until we've fully bubbled out of Duktape.
		 */
		DUK_D(DUK_DPRINT("execution timeout, throwing a RangeError"));
		thr->interrupt_init = 0;
		thr->interrupt_counter = 0;
		DUK_HEAP_CLEAR_INTERRUPT_RUNNING(thr->heap);
		DUK_ERROR(thr, DUK_ERR_RANGE_ERROR, "execution timeout");
	}
#endif  /* DUK_USE_EXEC_TIMEOUT_CHECK */

#if defined(DUK_USE_DEBUGGER_SUPPORT)
	if (DUK_HEAP_IS_DEBUGGER_ATTACHED(thr->heap)) {
		duk__interrupt_handle_debugger(thr, &immediate, &retval);
		act = thr->callstack + thr->callstack_top - 1;  /* relookup if changed */
	}
#endif  /* DUK_USE_DEBUGGER_SUPPORT */

	/*
	 *  Update the interrupt counter
	 */

	if (immediate) {
		/* Cause an interrupt after executing one instruction. */
		ctr = 1;
	}

	DUK_DDD(DUK_DDDPRINT("executor interrupt finished, cstop=%ld, pc=%ld, nextctr=%ld",
	                     (long) thr->callstack_top, (long) act->pc, (long) ctr));

	/* The counter value is one less than the init value: init value should
	 * indicate how many instructions are executed before interrupt.  To
	 * execute 1 instruction (after interrupt handler return), counter must
	 * be 0.
	 */
	thr->interrupt_init = ctr;
	thr->interrupt_counter = ctr - 1;
	DUK_HEAP_CLEAR_INTERRUPT_RUNNING(thr->heap);

	return retval;
}
#endif  /* DUK_USE_INTERRUPT_COUNTER */

/*
 *  Debugger handling for executor restart
 *
 *  Check for breakpoints, stepping, etc, and figure out if we should execute
 *  in checked or normal mode.  Note that we can't do this when an activation
 *  is created, because breakpoint status (and stepping status) may change
 *  later, so we must recheck every time we're executing an activation.
 */

#if defined(DUK_USE_DEBUGGER_SUPPORT)
DUK_LOCAL void duk__executor_handle_debugger(duk_hthread *thr, duk_activation *act, duk_hcompiledfunction *fun) {
	duk_heap *heap;
	duk_tval *tv_tmp;
	duk_hstring *filename;
	duk_small_uint_t bp_idx;
	duk_breakpoint **bp_active;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(act != NULL);
	DUK_ASSERT(fun != NULL);

	heap = thr->heap;
	bp_active = heap->dbg_breakpoints_active;
	act->flags &= ~DUK_ACT_FLAG_BREAKPOINT_ACTIVE;

	tv_tmp = duk_hobject_find_existing_entry_tval_ptr(thr->heap, (duk_hobject *) fun, DUK_HTHREAD_STRING_FILE_NAME(thr));
	if (tv_tmp && DUK_TVAL_IS_STRING(tv_tmp)) {
		filename = DUK_TVAL_GET_STRING(tv_tmp);

		/* Figure out all active breakpoints.  A breakpoint is
		 * considered active if the current function's fileName
		 * matches the breakpoint's fileName, AND there is no
		 * inner function that has matching line numbers
		 * (otherwise a breakpoint would be triggered both
		 * inside and outside of the inner function which would
		 * be confusing).  Example:
		 *
		 *     function foo() {
		 *         print('foo');
		 *         function bar() {    <-.  breakpoints in these
		 *             print('bar');     |  lines should not affect
		 *         }                   <-'  foo() execution
		 *         bar();
		 *     }
		 *
		 * We need a few things that are only available when
		 * debugger support is enabled: (1) a line range for
		 * each function, and (2) access to the function
		 * template to access the inner functions (and their
		 * line ranges).
		 *
		 * It's important to have a narrow match for active
		 * breakpoints so that we don't enter checked execution
		 * when that's not necessary.  For instance, if we're
		 * running inside a certain function and there's
		 * breakpoint outside in (after the call site), we
		 * don't want to slow down execution of the function.
		 */

		for (bp_idx = 0; bp_idx < heap->dbg_breakpoint_count; bp_idx++) {
			duk_breakpoint *bp = heap->dbg_breakpoints + bp_idx;
			duk_hobject **funcs, **funcs_end;
			duk_hcompiledfunction *inner_fun;
			duk_bool_t bp_match;

			if (bp->filename == filename &&
			    bp->line >= fun->start_line && bp->line <= fun->end_line) {
				bp_match = 1;
				DUK_DD(DUK_DDPRINT("breakpoint filename and line match: "
				                   "%s:%ld vs. %s (line %ld vs. %ld-%ld)",
				                   DUK_HSTRING_GET_DATA(bp->filename),
				                   (long) bp->line,
				                   DUK_HSTRING_GET_DATA(filename),
				                   (long) bp->line,
				                   (long) fun->start_line,
				                   (long) fun->end_line));

				funcs = DUK_HCOMPILEDFUNCTION_GET_FUNCS_BASE(thr->heap, fun);
				funcs_end = DUK_HCOMPILEDFUNCTION_GET_FUNCS_END(thr->heap, fun);
				while (funcs != funcs_end) {
					inner_fun = (duk_hcompiledfunction *) *funcs;
					DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION((duk_hobject *) inner_fun));
					if (bp->line >= inner_fun->start_line && bp->line <= inner_fun->end_line) {
						DUK_DD(DUK_DDPRINT("inner function masks ('captures') breakpoint"));
						bp_match = 0;
						break;
					}
					funcs++;
				}

				if (bp_match) {
					/* No need to check for size of bp_active list,
					 * it's always larger than maximum number of
					 * breakpoints.
					 */
					act->flags |= DUK_ACT_FLAG_BREAKPOINT_ACTIVE;
					*bp_active = heap->dbg_breakpoints + bp_idx;
					bp_active++;
				}
			}
		}
	}

	*bp_active = NULL;  /* terminate */

	DUK_DD(DUK_DDPRINT("ACTIVE BREAKPOINTS: %ld", (long) (bp_active - thr->heap->dbg_breakpoints_active)));

	/* Force pause if we were doing "step into" in another activation. */
	if (thr->heap->dbg_step_thread != NULL &&
	    thr->heap->dbg_step_type == DUK_STEP_TYPE_INTO &&
	    (thr->heap->dbg_step_thread != thr ||
	     thr->heap->dbg_step_csindex != thr->callstack_top - 1)) {
		DUK_D(DUK_DPRINT("STEP INTO ACTIVE, FORCE PAUSED"));
		DUK_HEAP_SET_PAUSED(thr->heap);
	}

	/* Force interrupt right away if we're paused or in "checked mode".
	 * Step out is handled by callstack unwind.
	 */
	if (act->flags & (DUK_ACT_FLAG_BREAKPOINT_ACTIVE) ||
	    thr->heap->dbg_paused ||
	    (thr->heap->dbg_step_type != DUK_STEP_TYPE_OUT &&
	     thr->heap->dbg_step_csindex == thr->callstack_top - 1)) {
		/* We'll need to interrupt early so recompute the init
		 * counter to reflect the number of bytecode instructions
		 * executed so that step counts for e.g. debugger rate
		 * limiting are accurate.
		 */
		DUK_ASSERT(thr->interrupt_counter <= thr->interrupt_init);
		thr->interrupt_init = thr->interrupt_init - thr->interrupt_counter;
		thr->interrupt_counter = 0;
	}
}
#endif  /* DUK_USE_DEBUGGER_SUPPORT */

/*
 *  Ecmascript bytecode executor.
 *
 *  Resume execution for the current thread from its current activation.
 *  Returns when execution would return from the entry level activation,
 *  leaving a single return value on top of the stack.  Function calls
 *  and thread resumptions are handled internally.  If an error occurs,
 *  a longjmp() with type DUK_LJ_TYPE_THROW is called on the entry level
 *  setjmp() jmpbuf.
 *
 *  Ecmascript function calls and coroutine resumptions are handled
 *  internally without recursive C calls.  Other function calls are
 *  handled using duk_handle_call(), increasing C recursion depth.
 *
 *  There are many other tricky control flow situations, such as:
 *
 *    - Break and continue (fast and slow)
 *    - Return (fast and slow)
 *    - Error throwing
 *    - Thread resume and yield
 *
 *  For more detailed notes, see doc/execution.rst.
 *
 *  Also see doc/code-issues.rst for discussion of setjmp(), longjmp(),
 *  and volatile.
 */

#define DUK__STRICT()       (DUK_HOBJECT_HAS_STRICT(&(fun)->obj))
#define DUK__REG(x)         (thr->valstack_bottom[(x)])
#define DUK__REGP(x)        (&thr->valstack_bottom[(x)])
#define DUK__CONST(x)       (DUK_HCOMPILEDFUNCTION_GET_CONSTS_BASE(thr->heap, fun)[(x)])
#define DUK__CONSTP(x)      (&DUK_HCOMPILEDFUNCTION_GET_CONSTS_BASE(thr->heap, fun)[(x)])
#define DUK__REGCONST(x)    ((x) < DUK_BC_REGLIMIT ? DUK__REG((x)) : DUK__CONST((x) - DUK_BC_REGLIMIT))
#define DUK__REGCONSTP(x)   ((x) < DUK_BC_REGLIMIT ? DUK__REGP((x)) : DUK__CONSTP((x) - DUK_BC_REGLIMIT))

#ifdef DUK_USE_VERBOSE_EXECUTOR_ERRORS
#define DUK__INTERNAL_ERROR(msg)  do { \
		DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, (msg)); \
	} while (0)
#else
#define DUK__INTERNAL_ERROR(msg)  do { \
		goto internal_error; \
	} while (0)
#endif

DUK_INTERNAL void duk_js_execute_bytecode(duk_hthread *exec_thr) {
	/* Entry level info.  Although these are assigned to before setjmp()
	 * a 'volatile' seems to be needed.  Note placement of "volatile" for
	 * pointers.  See doc/code-issues.rst for more discussion.
	 */
	duk_hthread * volatile entry_thread;   /* volatile copy of exec_thr */
	volatile duk_size_t entry_callstack_top;
	volatile duk_int_t entry_call_recursion_depth;
	duk_jmpbuf * volatile entry_jmpbuf_ptr;

	/* "hot" variables for interpretation -- not volatile, value not guaranteed in setjmp error handling */
	duk_hthread *thr;             /* stable */
	duk_activation *act;          /* semi-stable (ok as long as callstack not resized) */
	duk_hcompiledfunction *fun;   /* stable */
	duk_instr_t *bcode;           /* stable */
	/* 'consts' is computed on-the-fly */
	/* 'funcs' is quite rarely used, so no local for it */

	/* "hot" temps for interpretation -- not volatile, value not guaranteed in setjmp error handling */
	duk_uint_fast32_t ins;  /* XXX: check performance impact on x64 between fast/non-fast variant */

	/* jmpbuf */
	duk_jmpbuf jmpbuf;

#ifdef DUK_USE_INTERRUPT_COUNTER
	duk_int_t int_ctr;
#endif

#ifdef DUK_USE_ASSERTIONS
	duk_size_t valstack_top_base;    /* valstack top, should match before interpreting each op (no leftovers) */
#endif

	/* XXX: document assumptions on setjmp and volatile variables
	 * (see duk_handle_call()).
	 */

	/*
	 *  Preliminaries
	 */

	DUK_ASSERT(exec_thr != NULL);
	DUK_ASSERT(exec_thr->heap != NULL);
	DUK_ASSERT(exec_thr->heap->curr_thread != NULL);
	DUK_ASSERT_REFCOUNT_NONZERO_HEAPHDR((duk_heaphdr *) exec_thr);
	DUK_ASSERT(exec_thr->callstack_top >= 1);  /* at least one activation, ours */
	DUK_ASSERT(DUK_ACT_GET_FUNC(exec_thr->callstack + exec_thr->callstack_top - 1) != NULL);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(exec_thr->callstack + exec_thr->callstack_top - 1)));

	entry_thread = exec_thr;  /* volatile copy */
	thr = (duk_hthread *) entry_thread;
	entry_callstack_top = thr->callstack_top;
	entry_call_recursion_depth = thr->heap->call_recursion_depth;
	entry_jmpbuf_ptr = thr->heap->lj.jmpbuf_ptr;

	/*
	 *  Setjmp catchpoint setup.
	 *
	 *  Note: we currently assume that the setjmp() catchpoint is
	 *  not re-entrant (longjmp() cannot be called more than once
	 *  for a single setjmp()).
	 */

 reset_setjmp_catchpoint:

	DUK_ASSERT(thr != NULL);
	thr->heap->lj.jmpbuf_ptr = &jmpbuf;
	DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);

	if (DUK_SETJMP(thr->heap->lj.jmpbuf_ptr->jb)) {
		/*
		 *  Note: any local variables accessed here must have their value
		 *  assigned *before* the setjmp() call, OR they must be declared
		 *  volatile.  Otherwise their value is not guaranteed to be correct.
		 *
		 *  'thr' might seem to be a risky variable because it is changed
		 *  for yield and resume.  However, yield and resume are handled
		 *  using longjmp()s.
		 */

		duk_small_uint_t lj_ret;

		DUK_DDD(DUK_DDDPRINT("longjmp caught by bytecode executor"));

		/* Relookup 'thr': it's not volatile so its value is not
		 * guaranteed.  The heap->curr_thread value should always be
		 * valid here because longjmp callers don't switch threads,
		 * only the longjmp handler does that (even for RESUME and
		 * YIELD).
		 */
		DUK_ASSERT(entry_thread != NULL);
		thr = entry_thread->heap->curr_thread;

		/* XXX: signalling the need to shrink check (only if unwound) */

		/* Must be restored here to handle e.g. yields properly. */
		thr->heap->call_recursion_depth = entry_call_recursion_depth;

		/* Switch to caller's setjmp() catcher so that if an error occurs
		 * during error handling, it is always propagated outwards instead
		 * of causing an infinite loop in our own handler.
		 */

		DUK_DDD(DUK_DDDPRINT("restore jmpbuf_ptr: %p -> %p",
		                     (void *) ((thr && thr->heap) ? thr->heap->lj.jmpbuf_ptr : NULL),
		                     (void *) entry_jmpbuf_ptr));
		thr->heap->lj.jmpbuf_ptr = (duk_jmpbuf *) entry_jmpbuf_ptr;

		lj_ret = duk__handle_longjmp(thr, (duk_hthread *) entry_thread, (duk_size_t) entry_callstack_top);

		if (lj_ret == DUK__LONGJMP_RESTART) {
			/*
			 *  Restart bytecode execution, possibly with a changed thread.
			 */
			thr = thr->heap->curr_thread;
			goto reset_setjmp_catchpoint;
		} else if (lj_ret == DUK__LONGJMP_RETHROW) {
			/*
			 *  Rethrow error to calling state.
			 */

			/* thread may have changed (e.g. YIELD converted to THROW) */
			thr = thr->heap->curr_thread;

			DUK_ASSERT(thr->heap->lj.jmpbuf_ptr == entry_jmpbuf_ptr);

			duk_err_longjmp(thr);
			DUK_UNREACHABLE();
		} else {
			/*
			 *  Return from bytecode executor with a return value.
			 */
			DUK_ASSERT(lj_ret == DUK__LONGJMP_FINISHED);

			/* XXX: return assertions for valstack, callstack, catchstack */

			DUK_ASSERT(thr->heap->lj.jmpbuf_ptr == entry_jmpbuf_ptr);
			return;
		}
		DUK_UNREACHABLE();
	}

	/*
	 *  Restart execution by reloading thread state.
	 *
	 *  Note that 'thr' and any thread configuration may have changed,
	 *  so all local variables are suspect.
	 *
	 *  The number of local variables should be kept to a minimum: if
	 *  the variables are spilled, they will need to be loaded from
	 *  memory anyway.
	 */

 restart_execution:

	/* Lookup current thread; use the volatile 'entry_thread' for this to
	 * avoid clobber warnings.  (Any valid, reachable 'thr' value would be
	 * fine for this, so using 'entry_thread' is just to silence warnings.)
	 */
	thr = entry_thread->heap->curr_thread;
	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->callstack_top >= 1);
	DUK_ASSERT(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 1) != NULL);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_ACT_GET_FUNC(thr->callstack + thr->callstack_top - 1)));

	/* Assume interrupt init/counter are properly initialized here. */

	/* assume that thr->valstack_bottom has been set-up before getting here */
	act = thr->callstack + thr->callstack_top - 1;
	fun = (duk_hcompiledfunction *) DUK_ACT_GET_FUNC(act);
	DUK_ASSERT(fun != NULL);
	DUK_ASSERT(thr->valstack_top - thr->valstack_bottom == fun->nregs);
	bcode = DUK_HCOMPILEDFUNCTION_GET_CODE_BASE(thr->heap, fun);

#if defined(DUK_USE_DEBUGGER_SUPPORT)
	if (DUK_HEAP_IS_DEBUGGER_ATTACHED(thr->heap) && !thr->heap->dbg_processing) {
		thr->heap->dbg_processing = 1;
		duk__executor_handle_debugger(thr, act, fun);
		thr->heap->dbg_processing = 0;
	}
#endif  /* DUK_USE_DEBUGGER_SUPPORT */

	/* XXX: shrink check flag? */

	/*
	 *  Bytecode interpreter.
	 *
	 *  The interpreter must be very careful with memory pointers, as
	 *  many pointers are not guaranteed to be 'stable' and may be
	 *  reallocated and relocated on-the-fly quite easily (e.g. by a
	 *  memory allocation or a property access).
	 *
	 *  The following are assumed to have stable pointers:
	 *    - the current thread
	 *    - the current function
	 *    - the bytecode, constant table, inner function table of the
	 *      current function (as they are a part of the function allocation)
	 *
	 *  The following are assumed to have semi-stable pointers:
	 *    - the current activation entry: stable as long as callstack
	 *      is not changed (reallocated by growing or shrinking), or
	 *      by any garbage collection invocation (through finalizers)
	 *    - Note in particular that ANY DECREF can invalidate the
	 *      activation pointer
	 *
	 *  The following are not assumed to have stable pointers at all:
	 *    - the value stack (registers) of the current thread
	 *    - the catch stack of the current thread
	 *
	 *  See execution.txt for discussion.
	 */

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(act != NULL);
	DUK_ASSERT(fun != NULL);
	DUK_ASSERT(bcode != NULL);

	DUK_DD(DUK_DDPRINT("restarting execution, thr %p, act %p (idx %ld), fun %p, bcode %p, "
	                   "consts %p, funcs %p, lev %ld, regbot %ld, regtop %ld, catchstack_top=%ld, "
	                   "preventcount=%ld",
	                   (void *) thr,
	                   (void *) act,
	                   (long) (thr->callstack_top - 1),
	                   (void *) fun,
	                   (void *) bcode,
	                   (void *) DUK_HCOMPILEDFUNCTION_GET_CONSTS_BASE(thr->heap, fun),
	                   (void *) DUK_HCOMPILEDFUNCTION_GET_FUNCS_BASE(thr->heap, fun),
	                   (long) (thr->callstack_top - 1),
	                   (long) (thr->valstack_bottom - thr->valstack),
	                   (long) (thr->valstack_top - thr->valstack),
	                   (long) thr->catchstack_top,
	                   (long) thr->callstack_preventcount));

#ifdef DUK_USE_ASSERTIONS
	valstack_top_base = (duk_size_t) (thr->valstack_top - thr->valstack);
#endif

	for (;;) {
		DUK_ASSERT(thr->callstack_top >= 1);
		DUK_ASSERT(thr->valstack_top - thr->valstack_bottom == fun->nregs);
		DUK_ASSERT((duk_size_t) (thr->valstack_top - thr->valstack) == valstack_top_base);

		/* Executor interrupt counter check, used to implement breakpoints,
		 * debugging interface, execution timeouts, etc.  The counter is heap
		 * specific but is maintained in the current thread to make the check
		 * as fast as possible.  The counter is copied back to the heap struct
		 * whenever a thread switch occurs by the DUK_HEAP_SWITCH_THREAD() macro.
		 */
#ifdef DUK_USE_INTERRUPT_COUNTER
		int_ctr = thr->interrupt_counter;
		if (DUK_LIKELY(int_ctr > 0)) {
			thr->interrupt_counter = int_ctr - 1;
		} else {
			/* Trigger at zero or below */
			duk_small_uint_t exec_int_ret;

			exec_int_ret = duk__executor_interrupt(thr);
			if (exec_int_ret == DUK__INT_RESTART) {
				goto restart_execution;
			}
		}
#endif
#if defined(DUK_USE_INTERRUPT_COUNTER) && defined(DUK_USE_DEBUG)
		thr->heap->inst_count_exec++;
#endif

		/* Because ANY DECREF potentially invalidates 'act' now (through
		 * finalization), we need to re-lookup 'act' in almost every case.
		 *
		 * XXX: future work for performance optimization:
		 * This is not nice; it would be nice if the program counter was a
		 * behind a stable pointer.  For instance, put a raw bytecode pointer
		 * into duk_hthread struct (not into the callstack); since bytecode
		 * has a stable pointer this would work nicely.  Whenever a call is
		 * made, the bytecode pointer could be backed up as an integer index
		 * to the calling activation.  Perhaps add a macro for setting up a
		 * new activation (same as for setting up / switching threads)?
		 */

		act = thr->callstack + thr->callstack_top - 1;
		DUK_ASSERT(bcode + act->pc >= DUK_HCOMPILEDFUNCTION_GET_CODE_BASE(thr->heap, fun));
		DUK_ASSERT(bcode + act->pc < DUK_HCOMPILEDFUNCTION_GET_CODE_END(thr->heap, fun));

		DUK_DDD(DUK_DDDPRINT("executing bytecode: pc=%ld ins=0x%08lx, op=%ld, valstack_top=%ld/%ld, nregs=%ld  -->  %!I",
		                     (long) act->pc,
		                     (unsigned long) bcode[act->pc],
		                     (long) DUK_DEC_OP(bcode[act->pc]),
		                     (long) (thr->valstack_top - thr->valstack),
		                     (long) (thr->valstack_end - thr->valstack),
		                     (long) (fun ? fun->nregs : -1),
		                     (duk_instr_t) bcode[act->pc]));

#if defined(DUK_USE_ASSERTIONS)
		/* Quite heavy assert: check that valstack is in correctly
		 * initialized state.  Improper shuffle instructions can
		 * write beyond valstack_end so this check catches them in
		 * the act.
		 */
		{
			duk_tval *tv;
			tv = thr->valstack_top;
			while (tv != thr->valstack_end) {
				DUK_ASSERT(DUK_TVAL_IS_UNDEFINED_UNUSED(tv));
				tv++;
			}
		}
#endif

		ins = bcode[act->pc++];

		/* Typing: use duk_small_(u)int_fast_t when decoding small
		 * opcode fields (op, A, B, C) and duk_(u)int_fast_t when
		 * decoding larger fields (e.g. BC which is 18 bits).  Use
		 * unsigned variant by default, signed when the value is used
		 * in signed arithmetic.  Using variable names such as 'a', 'b',
		 * 'c', 'bc', etc makes it easier to spot typing mismatches.
		 */

		/* XXX: the best typing needs to be validated by perf measurement:
		 * e.g. using a small type which is the cast to a larger duk_idx_t
		 * may be slower than declaring the variable as a duk_idx_t in the
		 * first place.
		 */

		/* XXX: use macros for the repetitive tval/refcount handling. */

		switch ((int) DUK_DEC_OP(ins)) {
		/* XXX: switch cast? */

		case DUK_OP_LDREG: {
			duk_small_uint_fast_t a;
			duk_uint_fast_t bc;
			duk_tval tv_tmp;
			duk_tval *tv1, *tv2;

			a = DUK_DEC_A(ins); tv1 = DUK__REGP(a);
			bc = DUK_DEC_BC(ins); tv2 = DUK__REGP(bc);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
			DUK_TVAL_SET_TVAL(tv1, tv2);
			DUK_TVAL_INCREF(thr, tv1);
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
			break;
		}

		case DUK_OP_STREG: {
			duk_small_uint_fast_t a;
			duk_uint_fast_t bc;
			duk_tval tv_tmp;
			duk_tval *tv1, *tv2;

			a = DUK_DEC_A(ins); tv1 = DUK__REGP(a);
			bc = DUK_DEC_BC(ins); tv2 = DUK__REGP(bc);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv2);
			DUK_TVAL_SET_TVAL(tv2, tv1);
			DUK_TVAL_INCREF(thr, tv2);
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
			break;
		}

		case DUK_OP_LDCONST: {
			duk_small_uint_fast_t a;
			duk_uint_fast_t bc;
			duk_tval tv_tmp;
			duk_tval *tv1, *tv2;

			a = DUK_DEC_A(ins); tv1 = DUK__REGP(a);
			bc = DUK_DEC_BC(ins); tv2 = DUK__CONSTP(bc);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
			DUK_TVAL_SET_TVAL(tv1, tv2);
			DUK_TVAL_INCREF(thr, tv2);  /* may be e.g. string */
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
			break;
		}

		case DUK_OP_LDINT: {
			duk_small_uint_fast_t a;
			duk_int_fast_t bc;
			duk_tval tv_tmp;
			duk_tval *tv1;
#if defined(DUK_USE_FASTINT)
			duk_int32_t val;
#else
			duk_double_t val;
#endif

#if defined(DUK_USE_FASTINT)
			a = DUK_DEC_A(ins); tv1 = DUK__REGP(a);
			bc = DUK_DEC_BC(ins); val = (duk_int32_t) (bc - DUK_BC_LDINT_BIAS);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
			DUK_TVAL_SET_FASTINT_I32(tv1, val);
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
#else
			a = DUK_DEC_A(ins); tv1 = DUK__REGP(a);
			bc = DUK_DEC_BC(ins); val = (duk_double_t) (bc - DUK_BC_LDINT_BIAS);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
			DUK_TVAL_SET_NUMBER(tv1, val);
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
#endif
			break;
		}

		case DUK_OP_LDINTX: {
			duk_small_uint_fast_t a;
			duk_tval *tv1;
			duk_double_t val;

			/* LDINTX is not necessarily in FASTINT range, so
			 * no fast path for now.
			 *
			 * XXX: perhaps restrict LDINTX to fastint range, wider
			 * range very rarely needed.
			 */

			a = DUK_DEC_A(ins); tv1 = DUK__REGP(a);
			DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv1));
			val = DUK_TVAL_GET_NUMBER(tv1) * ((duk_double_t) (1L << DUK_BC_LDINTX_SHIFT)) +
			      (duk_double_t) DUK_DEC_BC(ins);
#if defined(DUK_USE_FASTINT)
			DUK_TVAL_SET_NUMBER_CHKFAST(tv1, val);
#else
			DUK_TVAL_SET_NUMBER(tv1, val);
#endif
			break;
		}

		case DUK_OP_MPUTOBJ:
		case DUK_OP_MPUTOBJI: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a;
			duk_tval *tv1;
			duk_hobject *obj;
			duk_uint_fast_t idx;
			duk_small_uint_fast_t count;

			/* A -> register of target object
			 * B -> first register of key/value pair list
			 * C -> number of key/value pairs
			 */

			a = DUK_DEC_A(ins); tv1 = DUK__REGP(a);
			DUK_ASSERT(DUK_TVAL_IS_OBJECT(tv1));
			obj = DUK_TVAL_GET_OBJECT(tv1);

			idx = (duk_uint_fast_t) DUK_DEC_B(ins);
			if (DUK_DEC_OP(ins) == DUK_OP_MPUTOBJI) {
				duk_tval *tv_ind = DUK__REGP(idx);
				DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_ind));
				idx = (duk_uint_fast_t) DUK_TVAL_GET_NUMBER(tv_ind);
			}

			count = (duk_small_uint_fast_t) DUK_DEC_C(ins);

#if defined(DUK_USE_EXEC_INDIRECT_BOUND_CHECK)
			if (DUK_UNLIKELY(idx + count * 2 > (duk_uint_fast_t) duk_get_top(ctx))) {
				/* XXX: use duk_is_valid_index() instead? */
				/* XXX: improve check; check against nregs, not against top */
				DUK__INTERNAL_ERROR("MPUTOBJ out of bounds");
			}
#endif

			duk_push_hobject(ctx, obj);

			while (count > 0) {
				/* XXX: faster initialization (direct access or better primitives) */

				duk_push_tval(ctx, DUK__REGP(idx));
				DUK_ASSERT(duk_is_string(ctx, -1));
				duk_push_tval(ctx, DUK__REGP(idx + 1));  /* -> [... obj key value] */
				duk_xdef_prop_wec(ctx, -3);              /* -> [... obj] */

				count--;
				idx += 2;
			}

			duk_pop(ctx);  /* [... obj] -> [...] */
			break;
		}

		case DUK_OP_MPUTARR:
		case DUK_OP_MPUTARRI: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a;
			duk_tval *tv1;
			duk_hobject *obj;
			duk_uint_fast_t idx;
			duk_small_uint_fast_t count;
			duk_uint32_t arr_idx;

			/* A -> register of target object
			 * B -> first register of value data (start_index, value1, value2, ..., valueN)
			 * C -> number of key/value pairs (N)
			 */

			a = DUK_DEC_A(ins); tv1 = DUK__REGP(a);
			DUK_ASSERT(DUK_TVAL_IS_OBJECT(tv1));
			obj = DUK_TVAL_GET_OBJECT(tv1);
			DUK_ASSERT(obj != NULL);

			idx = (duk_uint_fast_t) DUK_DEC_B(ins);
			if (DUK_DEC_OP(ins) == DUK_OP_MPUTARRI) {
				duk_tval *tv_ind = DUK__REGP(idx);
				DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_ind));
				idx = (duk_uint_fast_t) DUK_TVAL_GET_NUMBER(tv_ind);
			}

			count = (duk_small_uint_fast_t) DUK_DEC_C(ins);

#if defined(DUK_USE_EXEC_INDIRECT_BOUND_CHECK)
			if (idx + count + 1 > (duk_uint_fast_t) duk_get_top(ctx)) {
				/* XXX: use duk_is_valid_index() instead? */
				/* XXX: improve check; check against nregs, not against top */
				DUK__INTERNAL_ERROR("MPUTARR out of bounds");
			}
#endif

			tv1 = DUK__REGP(idx);
			DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv1));
			arr_idx = (duk_uint32_t) DUK_TVAL_GET_NUMBER(tv1);
			idx++;

			duk_push_hobject(ctx, obj);

			while (count > 0) {
				/* duk_xdef_prop() will define an own property without any array
				 * special behaviors.  We'll need to set the array length explicitly
				 * in the end.  For arrays with elisions, the compiler will emit an
				 * explicit SETALEN which will update the length.
				 */

				/* XXX: because we're dealing with 'own' properties of a fresh array,
				 * the array initializer should just ensure that the array has a large
				 * enough array part and write the values directly into array part,
				 * and finally set 'length' manually in the end (as already happens now).
				 */

				duk_push_tval(ctx, DUK__REGP(idx));          /* -> [... obj value] */
				duk_xdef_prop_index_wec(ctx, -2, arr_idx);   /* -> [... obj] */

				/* XXX: could use at least one fewer loop counters */
				count--;
				idx++;
				arr_idx++;
			}

			/* XXX: E5.1 Section 11.1.4 coerces the final length through
			 * ToUint32() which is odd but happens now as a side effect of
			 * 'arr_idx' type.
			 */
			duk_hobject_set_length(thr, obj, (duk_uint32_t) arr_idx);

			duk_pop(ctx);  /* [... obj] -> [...] */
			break;
		}

		case DUK_OP_NEW:
		case DUK_OP_NEWI: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_uint_fast_t idx;
			duk_small_uint_fast_t i;

			/* A -> unused (reserved for flags, for consistency with DUK_OP_CALL)
			 * B -> target register and start reg: constructor, arg1, ..., argN
			 *      (for DUK_OP_NEWI, 'b' is indirect)
			 * C -> num args (N)
			 */

			/* Note: duk_new() will call the constuctor using duk_handle_call().
			 * A constructor call prevents a yield from inside the constructor,
			 * even if the constructor is an Ecmascript function.
			 */

			/* XXX: unnecessary copying of values?  Just set 'top' to
			 * b + c, and let the return handling fix up the stack frame?
			 */

			idx = (duk_uint_fast_t) DUK_DEC_B(ins);
			if (DUK_DEC_OP(ins) == DUK_OP_NEWI) {
				duk_tval *tv_ind = DUK__REGP(idx);
				DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_ind));
				idx = (duk_uint_fast_t) DUK_TVAL_GET_NUMBER(tv_ind);
			}

#if defined(DUK_USE_EXEC_INDIRECT_BOUND_CHECK)
			if (idx + c + 1 > (duk_uint_fast_t) duk_get_top(ctx)) {
				/* XXX: use duk_is_valid_index() instead? */
				/* XXX: improve check; check against nregs, not against top */
				DUK__INTERNAL_ERROR("NEW out of bounds");
			}
#endif

			duk_require_stack(ctx, (duk_idx_t) c);
			duk_push_tval(ctx, DUK__REGP(idx));
			for (i = 0; i < c; i++) {
				duk_push_tval(ctx, DUK__REGP(idx + i + 1));
			}
			duk_new(ctx, (duk_idx_t) c);  /* [... constructor arg1 ... argN] -> [retval] */
			DUK_DDD(DUK_DDDPRINT("NEW -> %!iT", (duk_tval *) duk_get_tval(ctx, -1)));
			duk_replace(ctx, (duk_idx_t) idx);
			break;
		}

		case DUK_OP_REGEXP: {
#ifdef DUK_USE_REGEXP_SUPPORT
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);

			/* A -> target register
			 * B -> bytecode (also contains flags)
			 * C -> escaped source
			 */

			duk_push_tval(ctx, DUK__REGCONSTP(c));
			duk_push_tval(ctx, DUK__REGCONSTP(b));  /* -> [ ... escaped_source bytecode ] */
			duk_regexp_create_instance(thr);   /* -> [ ... regexp_instance ] */
			DUK_DDD(DUK_DDDPRINT("regexp instance: %!iT", (duk_tval *) duk_get_tval(ctx, -1)));
			duk_replace(ctx, (duk_idx_t) a);
#else
			/* The compiler should never emit DUK_OP_REGEXP if there is no
			 * regexp support.
			 */
			DUK__INTERNAL_ERROR("no regexp support");
#endif

			break;
		}

		case DUK_OP_CSREG:
		case DUK_OP_CSREGI: {
			/*
			 *  Assuming a register binds to a variable declared within this
			 *  function (a declarative binding), the 'this' for the call
			 *  setup is always 'undefined'.  E5 Section 10.2.1.1.6.
			 */

			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t b = DUK_DEC_B(ins);  /* restricted to regs */
			duk_uint_fast_t idx;

			/* A -> target register (A, A+1) for call setup
			 *      (for DUK_OP_CSREGI, 'a' is indirect)
			 * B -> register containing target function (not type checked here)
			 */

			/* XXX: direct manipulation, or duk_replace_tval() */

			/* Note: target registers a and a+1 may overlap with DUK__REGP(b).
			 * Careful here.
			 */

			idx = (duk_uint_fast_t) DUK_DEC_A(ins);
			if (DUK_DEC_OP(ins) == DUK_OP_CSREGI) {
				duk_tval *tv_ind = DUK__REGP(idx);
				DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_ind));
				idx = (duk_uint_fast_t) DUK_TVAL_GET_NUMBER(tv_ind);
			}

#if defined(DUK_USE_EXEC_INDIRECT_BOUND_CHECK)
			if (idx + 2 > (duk_uint_fast_t) duk_get_top(ctx)) {
				/* XXX: use duk_is_valid_index() instead? */
				/* XXX: improve check; check against nregs, not against top */
				DUK__INTERNAL_ERROR("CSREG out of bounds");
			}
#endif

			duk_push_tval(ctx, DUK__REGP(b));
			duk_replace(ctx, (duk_idx_t) idx);
			duk_push_undefined(ctx);
			duk_replace(ctx, (duk_idx_t) (idx + 1));
			break;
		}

		case DUK_OP_GETVAR: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_uint_fast_t bc = DUK_DEC_BC(ins);
			duk_tval *tv1;
			duk_hstring *name;

			tv1 = DUK__CONSTP(bc);
			DUK_ASSERT(DUK_TVAL_IS_STRING(tv1));
			name = DUK_TVAL_GET_STRING(tv1);
			DUK_ASSERT(name != NULL);
			DUK_DDD(DUK_DDDPRINT("GETVAR: '%!O'", (duk_heaphdr *) name));
			(void) duk_js_getvar_activation(thr, act, name, 1 /*throw*/);  /* -> [... val this] */

			duk_pop(ctx);  /* 'this' binding is not needed here */
			duk_replace(ctx, (duk_idx_t) a);
			break;
		}

		case DUK_OP_PUTVAR: {
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_uint_fast_t bc = DUK_DEC_BC(ins);
			duk_tval *tv1;
			duk_hstring *name;

			tv1 = DUK__CONSTP(bc);
			DUK_ASSERT(DUK_TVAL_IS_STRING(tv1));
			name = DUK_TVAL_GET_STRING(tv1);
			DUK_ASSERT(name != NULL);

			/* XXX: putvar takes a duk_tval pointer, which is awkward and
			 * should be reworked.
			 */

			tv1 = DUK__REGP(a);  /* val */
			duk_js_putvar_activation(thr, act, name, tv1, DUK__STRICT());
			break;
		}

		case DUK_OP_DECLVAR: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_tval *tv1;
			duk_hstring *name;
			duk_small_uint_t prop_flags;
			duk_bool_t is_func_decl;
			duk_bool_t is_undef_value;

			tv1 = DUK__REGCONSTP(b);
			DUK_ASSERT(DUK_TVAL_IS_STRING(tv1));
			name = DUK_TVAL_GET_STRING(tv1);
			DUK_ASSERT(name != NULL);

			is_undef_value = ((a & DUK_BC_DECLVAR_FLAG_UNDEF_VALUE) != 0);
			is_func_decl = ((a & DUK_BC_DECLVAR_FLAG_FUNC_DECL) != 0);

			/* XXX: declvar takes an duk_tval pointer, which is awkward and
			 * should be reworked.
			 */

			/* Compiler is responsible for selecting property flags (configurability,
			 * writability, etc).
			 */
			prop_flags = a & DUK_PROPDESC_FLAGS_MASK;

			if (is_undef_value) {
				duk_push_undefined(ctx);
			} else {
				duk_push_tval(ctx, DUK__REGCONSTP(c));
			}
			tv1 = duk_get_tval(ctx, -1);

			if (duk_js_declvar_activation(thr, act, name, tv1, prop_flags, is_func_decl)) {
				/* already declared, must update binding value */
				tv1 = duk_get_tval(ctx, -1);
				duk_js_putvar_activation(thr, act, name, tv1, DUK__STRICT());
			}

			duk_pop(ctx);
			break;
		}

		case DUK_OP_DELVAR: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_tval *tv1;
			duk_hstring *name;
			duk_bool_t rc;

			tv1 = DUK__REGCONSTP(b);
			DUK_ASSERT(DUK_TVAL_IS_STRING(tv1));
			name = DUK_TVAL_GET_STRING(tv1);
			DUK_ASSERT(name != NULL);
			DUK_DDD(DUK_DDDPRINT("DELVAR '%!O'", (duk_heaphdr *) name));
			rc = duk_js_delvar_activation(thr, act, name);

			duk_push_boolean(ctx, rc);
			duk_replace(ctx, (duk_idx_t) a);
			break;
		}

		case DUK_OP_CSVAR:
		case DUK_OP_CSVARI: {
			/* 'this' value:
			 * E5 Section 6.b.i
			 *
			 * The only (standard) case where the 'this' binding is non-null is when
			 *   (1) the variable is found in an object environment record, and
			 *   (2) that object environment record is a 'with' block.
			 *
			 */

			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_uint_fast_t idx;
			duk_tval *tv1;
			duk_hstring *name;

			tv1 = DUK__REGCONSTP(b);
			DUK_ASSERT(DUK_TVAL_IS_STRING(tv1));
			name = DUK_TVAL_GET_STRING(tv1);
			DUK_ASSERT(name != NULL);
			(void) duk_js_getvar_activation(thr, act, name, 1 /*throw*/);  /* -> [... val this] */

			/* Note: target registers a and a+1 may overlap with DUK__REGCONSTP(b)
			 * and DUK__REGCONSTP(c).  Careful here.
			 */

			idx = (duk_uint_fast_t) DUK_DEC_A(ins);
			if (DUK_DEC_OP(ins) == DUK_OP_CSVARI) {
				duk_tval *tv_ind = DUK__REGP(idx);
				DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_ind));
				idx = (duk_uint_fast_t) DUK_TVAL_GET_NUMBER(tv_ind);
			}

#if defined(DUK_USE_EXEC_INDIRECT_BOUND_CHECK)
			if (idx + 2 > (duk_uint_fast_t) duk_get_top(ctx)) {
				/* XXX: use duk_is_valid_index() instead? */
				/* XXX: improve check; check against nregs, not against top */
				DUK__INTERNAL_ERROR("CSVAR out of bounds");
			}
#endif

			duk_replace(ctx, (duk_idx_t) (idx + 1));  /* 'this' binding */
			duk_replace(ctx, (duk_idx_t) idx);        /* variable value (function, we hope, not checked here) */
			break;
		}

		case DUK_OP_CLOSURE: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_uint_fast_t bc = DUK_DEC_BC(ins);
			duk_hobject *fun_temp;

			/* A -> target reg
			 * BC -> inner function index
			 */

			DUK_DDD(DUK_DDDPRINT("CLOSURE to target register %ld, fnum %ld (count %ld)",
			                     (long) a, (long) bc, (long) DUK_HCOMPILEDFUNCTION_GET_FUNCS_COUNT(thr->heap, fun)));

			DUK_ASSERT_DISABLE(bc >= 0); /* unsigned */
			DUK_ASSERT((duk_uint_t) bc < (duk_uint_t) DUK_HCOMPILEDFUNCTION_GET_FUNCS_COUNT(thr->heap, fun));
			fun_temp = DUK_HCOMPILEDFUNCTION_GET_FUNCS_BASE(thr->heap, fun)[bc];
			DUK_ASSERT(fun_temp != NULL);
			DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(fun_temp));

			DUK_DDD(DUK_DDDPRINT("CLOSURE: function template is: %p -> %!O",
			                     (void *) fun_temp, (duk_heaphdr *) fun_temp));

			if (act->lex_env == NULL) {
				DUK_ASSERT(act->var_env == NULL);
				duk_js_init_activation_environment_records_delayed(thr, act);
			}
			DUK_ASSERT(act->lex_env != NULL);
			DUK_ASSERT(act->var_env != NULL);

			/* functions always have a NEWENV flag, i.e. they get a
			 * new variable declaration environment, so only lex_env
			 * matters here.
			 */
			duk_js_push_closure(thr,
			                    (duk_hcompiledfunction *) fun_temp,
			                    act->var_env,
			                    act->lex_env);
			duk_replace(ctx, (duk_idx_t) a);

			break;
		}

		case DUK_OP_GETPROP: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_tval *tv_obj;
			duk_tval *tv_key;
			duk_bool_t rc;

			/* A -> target reg
			 * B -> object reg/const (may be const e.g. in "'foo'[1]")
			 * C -> key reg/const
			 */

			tv_obj = DUK__REGCONSTP(b);
			tv_key = DUK__REGCONSTP(c);
			DUK_DDD(DUK_DDDPRINT("GETPROP: a=%ld obj=%!T, key=%!T",
			                     (long) a,
			                     (duk_tval *) DUK__REGCONSTP(b),
			                     (duk_tval *) DUK__REGCONSTP(c)));
			rc = duk_hobject_getprop(thr, tv_obj, tv_key);  /* -> [val] */
			DUK_UNREF(rc);  /* ignore */
			DUK_DDD(DUK_DDDPRINT("GETPROP --> %!T",
			                     (duk_tval *) duk_get_tval(ctx, -1)));
			tv_obj = NULL;  /* invalidated */
			tv_key = NULL;  /* invalidated */

			duk_replace(ctx, (duk_idx_t) a);    /* val */
			break;
		}

		case DUK_OP_PUTPROP: {
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_tval *tv_obj;
			duk_tval *tv_key;
			duk_tval *tv_val;
			duk_bool_t rc;

			/* A -> object reg
			 * B -> key reg/const
			 * C -> value reg/const
			 *
			 * Note: intentional difference to register arrangement
			 * of e.g. GETPROP; 'A' must contain a register-only value.
			 */

			tv_obj = DUK__REGP(a);
			tv_key = DUK__REGCONSTP(b);
			tv_val = DUK__REGCONSTP(c);
			DUK_DDD(DUK_DDDPRINT("PUTPROP: obj=%!T, key=%!T, val=%!T",
			                     (duk_tval *) DUK__REGP(a),
			                     (duk_tval *) DUK__REGCONSTP(b),
			                     (duk_tval *) DUK__REGCONSTP(c)));
			rc = duk_hobject_putprop(thr, tv_obj, tv_key, tv_val, DUK__STRICT());
			DUK_UNREF(rc);  /* ignore */
			DUK_DDD(DUK_DDDPRINT("PUTPROP --> obj=%!T, key=%!T, val=%!T",
			                     (duk_tval *) DUK__REGP(a),
			                     (duk_tval *) DUK__REGCONSTP(b),
			                     (duk_tval *) DUK__REGCONSTP(c)));
			tv_obj = NULL;  /* invalidated */
			tv_key = NULL;  /* invalidated */
			tv_val = NULL;  /* invalidated */

			break;
		}

		case DUK_OP_DELPROP: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_tval *tv_obj;
			duk_tval *tv_key;
			duk_bool_t rc;

			/* A -> result reg
			 * B -> object reg
			 * C -> key reg/const
			 */

			tv_obj = DUK__REGP(b);
			tv_key = DUK__REGCONSTP(c);
			rc = duk_hobject_delprop(thr, tv_obj, tv_key, DUK__STRICT());
			tv_obj = NULL;  /* invalidated */
			tv_key = NULL;  /* invalidated */

			duk_push_boolean(ctx, rc);
			duk_replace(ctx, (duk_idx_t) a);    /* result */
			break;
		}

		case DUK_OP_CSPROP:
		case DUK_OP_CSPROPI: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_uint_fast_t idx;
			duk_tval *tv_obj;
			duk_tval *tv_key;
			duk_bool_t rc;

			/* E5 Section 11.2.3, step 6.a.i */
			/* E5 Section 10.4.3 */

			/* XXX: allow object to be a const, e.g. in 'foo'.toString()?
			 * On the other hand, DUK_REGCONSTP() is slower and generates
			 * more code.
			 */

			tv_obj = DUK__REGP(b);
			tv_key = DUK__REGCONSTP(c);
			rc = duk_hobject_getprop(thr, tv_obj, tv_key);  /* -> [val] */
			DUK_UNREF(rc);  /* unused */
			tv_obj = NULL;  /* invalidated */
			tv_key = NULL;  /* invalidated */

			/* Note: target registers a and a+1 may overlap with DUK__REGP(b)
			 * and DUK__REGCONSTP(c).  Careful here.
			 */

			idx = (duk_uint_fast_t) DUK_DEC_A(ins);
			if (DUK_DEC_OP(ins) == DUK_OP_CSPROPI) {
				duk_tval *tv_ind = DUK__REGP(idx);
				DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_ind));
				idx = (duk_uint_fast_t) DUK_TVAL_GET_NUMBER(tv_ind);
			}

#if defined(DUK_USE_EXEC_INDIRECT_BOUND_CHECK)
			if (idx + 2 > (duk_uint_fast_t) duk_get_top(ctx)) {
				/* XXX: use duk_is_valid_index() instead? */
				/* XXX: improve check; check against nregs, not against top */
				DUK__INTERNAL_ERROR("CSPROP out of bounds");
			}
#endif

			duk_push_tval(ctx, DUK__REGP(b));         /* [ ... val obj ] */
			duk_replace(ctx, (duk_idx_t) (idx + 1));  /* 'this' binding */
			duk_replace(ctx, (duk_idx_t) idx);        /* val */
			break;
		}

		case DUK_OP_ADD:
		case DUK_OP_SUB:
		case DUK_OP_MUL:
		case DUK_OP_DIV:
		case DUK_OP_MOD: {
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_small_uint_fast_t op = DUK_DEC_OP(ins);

			if (op == DUK_OP_ADD) {
				/*
				 *  Handling DUK_OP_ADD this way is more compact (experimentally)
				 *  than a separate case with separate argument decoding.
				 */
				duk__vm_arith_add(thr, DUK__REGCONSTP(b), DUK__REGCONSTP(c), a);
			} else {
				duk__vm_arith_binary_op(thr, DUK__REGCONSTP(b), DUK__REGCONSTP(c), a, op);
			}
			break;
		}

		case DUK_OP_BAND:
		case DUK_OP_BOR:
		case DUK_OP_BXOR:
		case DUK_OP_BASL:
		case DUK_OP_BLSR:
		case DUK_OP_BASR: {
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_small_uint_fast_t op = DUK_DEC_OP(ins);

			duk__vm_bitwise_binary_op(thr, DUK__REGCONSTP(b), DUK__REGCONSTP(c), a, op);
			break;
		}

		case DUK_OP_EQ:
		case DUK_OP_NEQ: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_bool_t tmp;

			/* E5 Sections 11.9.1, 11.9.3 */
			tmp = duk_js_equals(thr, DUK__REGCONSTP(b), DUK__REGCONSTP(c));
			if (DUK_DEC_OP(ins) == DUK_OP_NEQ) {
				tmp = !tmp;
			}
			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, (duk_idx_t) a);
			break;
		}

		case DUK_OP_SEQ:
		case DUK_OP_SNEQ: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_bool_t tmp;

			/* E5 Sections 11.9.1, 11.9.3 */
			tmp = duk_js_strict_equals(DUK__REGCONSTP(b), DUK__REGCONSTP(c));
			if (DUK_DEC_OP(ins) == DUK_OP_SNEQ) {
				tmp = !tmp;
			}
			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, (duk_idx_t) a);
			break;
		}

		/* Note: combining comparison ops must be done carefully because
		 * of uncomparable values (NaN): it's not necessarily true that
		 * (x >= y) === !(x < y).  Also, evaluation order matters, and
		 * although it would only seem to affect the compiler this is
		 * actually not the case, because there are also run-time coercions
		 * of the arguments (with potential side effects).
		 *
		 * XXX: can be combined; check code size.
		 */

		case DUK_OP_GT: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_bool_t tmp;

			/* x > y  -->  y < x */
			tmp = duk_js_compare_helper(thr,
			                            DUK__REGCONSTP(c),  /* y */
			                            DUK__REGCONSTP(b),  /* x */
			                            0);                 /* flags */

			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, (duk_idx_t) a);
			break;
		}

		case DUK_OP_GE: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_bool_t tmp;

			/* x >= y  -->  not (x < y) */
			tmp = duk_js_compare_helper(thr,
			                            DUK__REGCONSTP(b),  /* x */
			                            DUK__REGCONSTP(c),  /* y */
			                            DUK_COMPARE_FLAG_EVAL_LEFT_FIRST |
			                            DUK_COMPARE_FLAG_NEGATE);  /* flags */

			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, (duk_idx_t) a);
			break;
		}

		case DUK_OP_LT: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_bool_t tmp;

			/* x < y */
			tmp = duk_js_compare_helper(thr,
			                            DUK__REGCONSTP(b),  /* x */
			                            DUK__REGCONSTP(c),  /* y */
			                            DUK_COMPARE_FLAG_EVAL_LEFT_FIRST);  /* flags */

			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, (duk_idx_t) a);
			break;
		}

		case DUK_OP_LE: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_bool_t tmp;

			/* x <= y  -->  not (x > y)  -->  not (y < x) */
			tmp = duk_js_compare_helper(thr,
			                            DUK__REGCONSTP(c),  /* y */
			                            DUK__REGCONSTP(b),  /* x */
			                            DUK_COMPARE_FLAG_NEGATE);  /* flags */

			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, (duk_idx_t) a);
			break;
		}

		case DUK_OP_IF: {
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_bool_t tmp;

			tmp = duk_js_toboolean(DUK__REGCONSTP(b));
			if (tmp == (duk_bool_t) a) {
				/* if boolean matches A, skip next inst */
				act->pc++;
			} else {
				;
			}
			break;
		}

		case DUK_OP_JUMP: {
			duk_int_fast_t abc = DUK_DEC_ABC(ins);

			act->pc += abc - DUK_BC_JUMP_BIAS;
			break;
		}

		case DUK_OP_RETURN: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			/* duk_small_uint_fast_t c = DUK_DEC_C(ins); */
			duk_tval *tv_val;

			/* A -> flags
			 * B -> return value reg/const
			 * C -> currently unused
			 */

			/* A fast return avoids full longjmp handling for a set of
			 * scenarios which hopefully represents the common cases.
			 * The compiler is responsible for emitting fast returns
			 * only when they are safe.  Currently this means that there
			 * is nothing on the catch stack (not even label catchers).
			 * The speed advantage of fast returns (avoiding longjmp) is
			 * not very high, around 10-15%.
			 */
#if 0  /* XXX: Disabled for 1.0 release */
			if (a & DUK_BC_RETURN_FLAG_FAST) {
				DUK_DDD(DUK_DDDPRINT("FASTRETURN attempt a=%ld b=%ld", (long) a, (long) b));

				if (duk__handle_fast_return(thr,
				                            (a & DUK_BC_RETURN_FLAG_HAVE_RETVAL) ? DUK__REGCONSTP(b) : NULL,
				                            entry_thread,
				                            entry_callstack_top)) {
					DUK_DDD(DUK_DDDPRINT("FASTRETURN success a=%ld b=%ld", (long) a, (long) b));
					goto restart_execution;
				}
			}
#endif

			/* No fast return, slow path. */
			DUK_DDD(DUK_DDDPRINT("SLOWRETURN a=%ld b=%ld", (long) a, (long) b));

			if (a & DUK_BC_RETURN_FLAG_HAVE_RETVAL) {
				tv_val = DUK__REGCONSTP(b);
#if defined(DUK_USE_FASTINT)
				/* Explicit check for fastint downgrade.  Do
				 * it also for consts for now, which is odd
				 * but harmless.
				 */
				/* XXX: restrict to reg values only? */

				DUK_TVAL_CHKFAST_INPLACE(tv_val);
#endif
				duk_push_tval(ctx, tv_val);
			} else {
				duk_push_undefined(ctx);
			}

			duk_err_setup_heap_ljstate(thr, DUK_LJ_TYPE_RETURN);

			DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);  /* in bytecode executor, should always be set */
			duk_err_longjmp(thr);
			DUK_UNREACHABLE();
			break;
		}

		case DUK_OP_CALL:
		case DUK_OP_CALLI: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_uint_fast_t idx;
			duk_small_uint_t call_flags;
			duk_small_uint_t flag_tailcall;
			duk_small_uint_t flag_evalcall;
			duk_tval *tv_func;
			duk_hobject *obj_func;
			duk_bool_t setup_rc;
			duk_idx_t num_stack_args;

			/* A -> flags
			 * B -> base register for call (base -> func, base+1 -> this, base+2 -> arg1 ... base+2+N-1 -> argN)
			 *      (for DUK_OP_CALLI, 'b' is indirect)
			 * C -> nargs
			 */

			/* these are not necessarily 0 or 1 (may be other non-zero), that's ok */
			flag_tailcall = (a & DUK_BC_CALL_FLAG_TAILCALL);
			flag_evalcall = (a & DUK_BC_CALL_FLAG_EVALCALL);

			idx = (duk_uint_fast_t) DUK_DEC_B(ins);
			if (DUK_DEC_OP(ins) == DUK_OP_CALLI) {
				duk_tval *tv_ind = DUK__REGP(idx);
				DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_ind));
				idx = (duk_uint_fast_t) DUK_TVAL_GET_NUMBER(tv_ind);
			}

#if defined(DUK_USE_EXEC_INDIRECT_BOUND_CHECK)
			if (!duk_is_valid_index(ctx, (duk_idx_t) idx)) {
				/* XXX: improve check; check against nregs, not against top */
				DUK__INTERNAL_ERROR("CALL out of bounds");
			}
#endif

			/*
			 *  To determine whether to use an optimized Ecmascript-to-Ecmascript
			 *  call, we need to know whether the final, non-bound function is an
			 *  Ecmascript function.
			 *
			 *  This is now implemented so that we start to do an ecma-to-ecma call
			 *  setup which will resolve the bound chain as the first thing.  If the
			 *  final function is not eligible, the return value indicates that the
			 *  ecma-to-ecma call is not possible.  The setup will overwrite the call
			 *  target at DUK__REGP(idx) with the final, non-bound function (which
			 *  may be a lightfunc), and fudge arguments if necessary.
			 *
			 *  XXX: If an ecma-to-ecma call is not possible, this initial call
			 *  setup will do bound function chain resolution but won't do the
			 *  "effective this binding" resolution which is quite confusing.
			 *  Perhaps add a helper for doing bound function and effective this
			 *  binding resolution - and call that explicitly?  Ecma-to-ecma call
			 *  setup and normal function handling can then assume this prestep has
			 *  been done by the caller.
			 */

			duk_set_top(ctx, (duk_idx_t) (idx + c + 2));   /* [ ... func this arg1 ... argN ] */

			call_flags = 0;
			if (flag_tailcall) {
				/* We request a tailcall, but in some corner cases
				 * call handling can decide that a tailcall is
				 * actually not possible.
				 * See: test-bug-tailcall-preventyield-assert.c.
				 */
				call_flags |= DUK_CALL_FLAG_IS_TAILCALL;
			}

			/* Compared to duk_handle_call():
			 *   - protected call: never
			 *   - ignore recursion limit: never
			 */
			num_stack_args = c;
			setup_rc = duk_handle_ecma_call_setup(thr,
			                                      num_stack_args,
			                                      call_flags);

			if (setup_rc) {
				/* Ecma-to-ecma call possible, may or may not be a tailcall.
				 * Avoid C recursion by being clever.
				 */
				DUK_DDD(DUK_DDDPRINT("ecma-to-ecma call setup possible, restart execution"));
				goto restart_execution;
			}

			DUK_DDD(DUK_DDDPRINT("ecma-to-ecma call not possible, target is native (may be lightfunc)"));

			/* Recompute argument count: bound function handling may have shifted. */
			num_stack_args = duk_get_top(ctx) - (idx + 2);
			DUK_DDD(DUK_DDDPRINT("recomputed arg count: %ld\n", (long) num_stack_args));

			tv_func = DUK__REGP(idx);  /* Relookup if relocated */
			if (DUK_TVAL_IS_LIGHTFUNC(tv_func)) {
				call_flags = 0;  /* not protected, respect reclimit, not constructor */

				/* There is no eval() special handling here: eval() is never
				 * automatically converted to a lightfunc.
				 */
				DUK_ASSERT(DUK_TVAL_GET_LIGHTFUNC_FUNCPTR(tv_func) != duk_bi_global_object_eval);

				duk_handle_call(thr,
				                num_stack_args,
				                call_flags);

				/* duk_js_call.c is required to restore the stack reserve
				 * so we only need to reset the top.
				 */
				duk_set_top(ctx, (duk_idx_t) fun->nregs);

				/* No need to reinit setjmp() catchpoint, as call handling
				 * will store and restore our state.
				 */
			} else {
				/* Call setup checks callability. */
				DUK_ASSERT(DUK_TVAL_IS_OBJECT(tv_func));
				obj_func = DUK_TVAL_GET_OBJECT(tv_func);
				DUK_ASSERT(obj_func != NULL);
				DUK_ASSERT(!DUK_HOBJECT_HAS_BOUND(obj_func));

				/*
				 *  Other cases, use C recursion.
				 *
				 *  If a tailcall was requested we ignore it and execute a normal call.
				 *  Since Duktape 0.11.0 the compiler emits a RETURN opcode even after
				 *  a tailcall to avoid test-bug-tailcall-thread-yield-resume.js.
				 *
				 *  Direct eval call: (1) call target (before following bound function
				 *  chain) is the built-in eval() function, and (2) call was made with
				 *  the identifier 'eval'.
				 */

				call_flags = 0;  /* not protected, respect reclimit, not constructor */

				if (DUK_HOBJECT_IS_NATIVEFUNCTION(obj_func) &&
				    ((duk_hnativefunction *) obj_func)->func == duk_bi_global_object_eval) {
					if (flag_evalcall) {
						DUK_DDD(DUK_DDDPRINT("call target is eval, call identifier was 'eval' -> direct eval"));
						call_flags |= DUK_CALL_FLAG_DIRECT_EVAL;
					} else {
						DUK_DDD(DUK_DDDPRINT("call target is eval, call identifier was not 'eval' -> indirect eval"));
					}
				}

				duk_handle_call(thr,
				                num_stack_args,
				                call_flags);

				/* duk_js_call.c is required to restore the stack reserve
				 * so we only need to reset the top.
				 */
				duk_set_top(ctx, (duk_idx_t) fun->nregs);

				/* No need to reinit setjmp() catchpoint, as call handling
				 * will store and restore our state.
				 */
			}

#if defined(DUK_USE_DEBUGGER_SUPPORT)
			/* When debugger is enabled, we need to recheck the activation
			 * status after returning.
			 */
			goto restart_execution;
#endif
			break;
		}

		case DUK_OP_TRYCATCH: {
			duk_context *ctx = (duk_context *) thr;
			duk_catcher *cat;
			duk_tval *tv1;
			duk_small_uint_fast_t a;
			duk_uint_fast_t bc;

			/* A -> flags
			 * BC -> reg_catch; base register for two registers used both during
			 *       trycatch setup and when catch is triggered
			 *
			 *      If DUK_BC_TRYCATCH_FLAG_CATCH_BINDING set:
			 *          reg_catch + 0: catch binding variable name (string).
			 *          Automatic declarative environment is established for
			 *          the duration of the 'catch' clause.
			 *
			 *      If DUK_BC_TRYCATCH_FLAG_WITH_BINDING set:
			 *          reg_catch + 0: with 'target value', which is coerced to
			 *          an object and then used as a bindind object for an
			 *          environment record.  The binding is initialized here, for
			 *          the 'try' clause.
			 *
			 * Note that a TRYCATCH generated for a 'with' statement has no
			 * catch or finally parts.
			 */

			/* XXX: TRYCATCH handling should be reworked to avoid creating
			 * an explicit scope unless it is actually needed (e.g. function
			 * instances or eval is executed inside the catch block).  This
			 * rework is not trivial because the compiler doesn't have an
			 * intermediate representation.  When the rework is done, the
			 * opcode format can also be made more straightforward.
			 */

			/* XXX: side effect handling is quite awkward here */

			DUK_DDD(DUK_DDDPRINT("TRYCATCH: reg_catch=%ld, have_catch=%ld, "
			                     "have_finally=%ld, catch_binding=%ld, with_binding=%ld (flags=0x%02lx)",
			                     (long) DUK_DEC_BC(ins),
			                     (long) (DUK_DEC_A(ins) & DUK_BC_TRYCATCH_FLAG_HAVE_CATCH ? 1 : 0),
			                     (long) (DUK_DEC_A(ins) & DUK_BC_TRYCATCH_FLAG_HAVE_FINALLY ? 1 : 0),
			                     (long) (DUK_DEC_A(ins) & DUK_BC_TRYCATCH_FLAG_CATCH_BINDING ? 1 : 0),
			                     (long) (DUK_DEC_A(ins) & DUK_BC_TRYCATCH_FLAG_WITH_BINDING ? 1 : 0),
			                     (unsigned long) DUK_DEC_A(ins)));

			a = DUK_DEC_A(ins);
			bc = DUK_DEC_BC(ins);

			DUK_ASSERT(thr->callstack_top >= 1);

			/* 'with' target must be created first, in case we run out of memory */
			/* XXX: refactor out? */

			if (a & DUK_BC_TRYCATCH_FLAG_WITH_BINDING) {
				DUK_DDD(DUK_DDDPRINT("need to initialize a with binding object"));

				if (act->lex_env == NULL) {
					DUK_ASSERT(act->var_env == NULL);
					DUK_DDD(DUK_DDDPRINT("delayed environment initialization"));

					/* must relookup act in case of side effects */
					duk_js_init_activation_environment_records_delayed(thr, act);
					act = thr->callstack + thr->callstack_top - 1;
				}
				DUK_ASSERT(act->lex_env != NULL);
				DUK_ASSERT(act->var_env != NULL);

				(void) duk_push_object_helper(ctx,
				                              DUK_HOBJECT_FLAG_EXTENSIBLE |
				                              DUK_HOBJECT_CLASS_AS_FLAGS(DUK_HOBJECT_CLASS_OBJENV),
				                              -1);  /* no prototype, updated below */

				duk_push_tval(ctx, DUK__REGP(bc));
				duk_to_object(ctx, -1);
				duk_dup(ctx, -1);

				/* [ ... env target ] */
				/* [ ... env target target ] */

				duk_xdef_prop_stridx(thr, -3, DUK_STRIDX_INT_TARGET, DUK_PROPDESC_FLAGS_NONE);
				duk_xdef_prop_stridx(thr, -2, DUK_STRIDX_INT_THIS, DUK_PROPDESC_FLAGS_NONE);  /* always provideThis=true */

				/* [ ... env ] */

				DUK_DDD(DUK_DDDPRINT("environment for with binding: %!iT",
				                     (duk_tval *) duk_get_tval(ctx, -1)));
			}

			/* allocate catcher and populate it (should be atomic) */

			duk_hthread_catchstack_grow(thr);
			cat = thr->catchstack + thr->catchstack_top;
			DUK_ASSERT(thr->catchstack_top + 1 <= thr->catchstack_size);
			thr->catchstack_top++;

			cat->flags = DUK_CAT_TYPE_TCF;
			cat->h_varname = NULL;

			if (a & DUK_BC_TRYCATCH_FLAG_HAVE_CATCH) {
				cat->flags |= DUK_CAT_FLAG_CATCH_ENABLED;
			}
			if (a & DUK_BC_TRYCATCH_FLAG_HAVE_FINALLY) {
				cat->flags |= DUK_CAT_FLAG_FINALLY_ENABLED;
			}
			if (a & DUK_BC_TRYCATCH_FLAG_CATCH_BINDING) {
				DUK_DDD(DUK_DDDPRINT("catch binding flag set to catcher"));
				cat->flags |= DUK_CAT_FLAG_CATCH_BINDING_ENABLED;
				tv1 = DUK__REGP(bc);
				DUK_ASSERT(DUK_TVAL_IS_STRING(tv1));

				/* borrowed reference; although 'tv1' comes from a register,
				 * its value was loaded using LDCONST so the constant will
				 * also exist and be reachable.
				 */
				cat->h_varname = DUK_TVAL_GET_STRING(tv1);
			} else if (a & DUK_BC_TRYCATCH_FLAG_WITH_BINDING) {
				/* env created above to stack top */
				duk_hobject *new_env;

				DUK_DDD(DUK_DDDPRINT("lexenv active flag set to catcher"));
				cat->flags |= DUK_CAT_FLAG_LEXENV_ACTIVE;

				DUK_DDD(DUK_DDDPRINT("activating object env: %!iT",
				                     (duk_tval *) duk_get_tval(ctx, -1)));
				DUK_ASSERT(act->lex_env != NULL);
				new_env = duk_get_hobject(ctx, -1);
				DUK_ASSERT(new_env != NULL);

				act = thr->callstack + thr->callstack_top - 1;  /* relookup (side effects) */
				DUK_HOBJECT_SET_PROTOTYPE_UPDREF(thr, new_env, act->lex_env);

				act = thr->callstack + thr->callstack_top - 1;  /* relookup (side effects) */
				act->lex_env = new_env;
				DUK_HOBJECT_INCREF(thr, new_env);
				duk_pop(ctx);
			} else {
				;
			}

			cat = thr->catchstack + thr->catchstack_top - 1;  /* relookup (side effects) */
			cat->callstack_index = thr->callstack_top - 1;
			cat->pc_base = act->pc;  /* pre-incremented, points to first jump slot */
			cat->idx_base = (duk_size_t) (thr->valstack_bottom - thr->valstack) + bc;

			DUK_DDD(DUK_DDDPRINT("TRYCATCH catcher: flags=0x%08lx, callstack_index=%ld, pc_base=%ld, "
			                     "idx_base=%ld, h_varname=%!O",
			                     (unsigned long) cat->flags, (long) cat->callstack_index,
			                     (long) cat->pc_base, (long) cat->idx_base, (duk_heaphdr *) cat->h_varname));

			act->pc += 2;  /* skip jump slots */
			break;
		}

		/* Pre/post inc/dec for register variables, important for loops. */
		case DUK_OP_PREINCR:
		case DUK_OP_PREDECR:
		case DUK_OP_POSTINCR:
		case DUK_OP_POSTDECR: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_uint_fast_t bc = DUK_DEC_BC(ins);
			duk_tval *tv1, *tv2;
			duk_tval tv_tmp;
			duk_double_t x, y, z;

			/* Two lowest bits of opcode are used to distinguish
			 * variants.  Bit 0 = inc(0)/dec(1), bit 1 = pre(0)/post(1).
			 */
			DUK_ASSERT((DUK_OP_PREINCR & 0x03) == 0x00);
			DUK_ASSERT((DUK_OP_PREDECR & 0x03) == 0x01);
			DUK_ASSERT((DUK_OP_POSTINCR & 0x03) == 0x02);
			DUK_ASSERT((DUK_OP_POSTDECR & 0x03) == 0x03);

			tv1 = DUK__REGP(bc);
#if defined(DUK_USE_FASTINT)
			if (DUK_TVAL_IS_FASTINT(tv1)) {
				duk_int64_t x_fi, y_fi, z_fi;
				x_fi = DUK_TVAL_GET_FASTINT(tv1);
				if (ins & DUK_ENC_OP(0x01)) {
					if (x_fi == DUK_FASTINT_MIN) {
						goto skip_fastint;
					}
					y_fi = x_fi - 1;
				} else {
					if (x_fi == DUK_FASTINT_MAX) {
						goto skip_fastint;
					}
					y_fi = x_fi + 1;
				}

				DUK_TVAL_SET_FASTINT(tv1, y_fi);  /* no need for refcount update */

				tv2 = DUK__REGP(a);
				DUK_TVAL_SET_TVAL(&tv_tmp, tv2);
				z_fi = (ins & DUK_ENC_OP(0x02)) ? x_fi : y_fi;
				DUK_TVAL_SET_FASTINT(tv2, z_fi);    /* no need for incref */
				DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
				break;
			}
		 skip_fastint:
#endif
			if (DUK_TVAL_IS_NUMBER(tv1)) {
				/* Fast path for the case where the register
				 * is a number (e.g. loop counter).
				 */

				x = DUK_TVAL_GET_NUMBER(tv1);
				if (ins & DUK_ENC_OP(0x01)) {
					y = x - 1.0;
				} else {
					y = x + 1.0;
				}

				DUK_TVAL_SET_NUMBER(tv1, y);  /* no need for refcount update */
			} else {
				x = duk_to_number(ctx, bc);

				if (ins & DUK_ENC_OP(0x01)) {
					y = x - 1.0;
				} else {
					y = x + 1.0;
				}

				duk_push_number(ctx, y);
				duk_replace(ctx, bc);
			}

			tv2 = DUK__REGP(a);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv2);
			z = (ins & DUK_ENC_OP(0x02)) ? x : y;
			DUK_TVAL_SET_NUMBER(tv2, z);    /* no need for incref */
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
			break;
		}

		/* Preinc/predec for var-by-name, slow path. */
		case DUK_OP_PREINCV:
		case DUK_OP_PREDECV:
		case DUK_OP_POSTINCV:
		case DUK_OP_POSTDECV: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_uint_fast_t bc = DUK_DEC_BC(ins);
			duk_double_t x, y;
			duk_tval *tv1;
			duk_hstring *name;

			/* Two lowest bits of opcode are used to distinguish
			 * variants.  Bit 0 = inc(0)/dec(1), bit 1 = pre(0)/post(1).
			 */
			DUK_ASSERT((DUK_OP_PREINCV & 0x03) == 0x00);
			DUK_ASSERT((DUK_OP_PREDECV & 0x03) == 0x01);
			DUK_ASSERT((DUK_OP_POSTINCV & 0x03) == 0x02);
			DUK_ASSERT((DUK_OP_POSTDECV & 0x03) == 0x03);

			tv1 = DUK__CONSTP(bc);
			DUK_ASSERT(DUK_TVAL_IS_STRING(tv1));
			name = DUK_TVAL_GET_STRING(tv1);
			DUK_ASSERT(name != NULL);
			(void) duk_js_getvar_activation(thr, act, name, 1 /*throw*/);  /* -> [... val this] */

			/* XXX: fastint fast path would be very useful here */

			x = duk_to_number(ctx, -2);
			duk_pop_2(ctx);
			if (ins & DUK_ENC_OP(0x01)) {
				y = x - 1.0;
			} else {
				y = x + 1.0;
			}

			duk_push_number(ctx, y);
			tv1 = duk_get_tval(ctx, -1);
			DUK_ASSERT(tv1 != NULL);
			duk_js_putvar_activation(thr, act, name, tv1, DUK__STRICT());
			duk_pop(ctx);

			duk_push_number(ctx, (ins & DUK_ENC_OP(0x02)) ? x : y);
			duk_replace(ctx, (duk_idx_t) a);
			break;
		}

		/* Preinc/predec for object properties. */
		case DUK_OP_PREINCP:
		case DUK_OP_PREDECP:
		case DUK_OP_POSTINCP:
		case DUK_OP_POSTDECP: {
			duk_context *ctx = (duk_context *) thr;
			duk_small_uint_fast_t a = DUK_DEC_A(ins);
			duk_small_uint_fast_t b = DUK_DEC_B(ins);
			duk_small_uint_fast_t c = DUK_DEC_C(ins);
			duk_tval *tv_obj;
			duk_tval *tv_key;
			duk_tval *tv_val;
			duk_bool_t rc;
			duk_double_t x, y;

			/* A -> target reg
			 * B -> object reg/const (may be const e.g. in "'foo'[1]")
			 * C -> key reg/const
			 */

			/* Two lowest bits of opcode are used to distinguish
			 * variants.  Bit 0 = inc(0)/dec(1), bit 1 = pre(0)/post(1).
			 */
			DUK_ASSERT((DUK_OP_PREINCP & 0x03) == 0x00);
			DUK_ASSERT((DUK_OP_PREDECP & 0x03) == 0x01);
			DUK_ASSERT((DUK_OP_POSTINCP & 0x03) == 0x02);
			DUK_ASSERT((DUK_OP_POSTDECP & 0x03) == 0x03);

			tv_obj = DUK__REGCONSTP(b);
			tv_key = DUK__REGCONSTP(c);
			rc = duk_hobject_getprop(thr, tv_obj, tv_key);  /* -> [val] */
			DUK_UNREF(rc);  /* ignore */
			tv_obj = NULL;  /* invalidated */
			tv_key = NULL;  /* invalidated */

			x = duk_to_number(ctx, -1);
			duk_pop(ctx);
			if (ins & DUK_ENC_OP(0x01)) {
				y = x - 1.0;
			} else {
				y = x + 1.0;
			}

			duk_push_number(ctx, y);
			tv_val = duk_get_tval(ctx, -1);
			DUK_ASSERT(tv_val != NULL);
			tv_obj = DUK__REGCONSTP(b);
			tv_key = DUK__REGCONSTP(c);
			rc = duk_hobject_putprop(thr, tv_obj, tv_key, tv_val, DUK__STRICT());
			DUK_UNREF(rc);  /* ignore */
			tv_obj = NULL;  /* invalidated */
			tv_key = NULL;  /* invalidated */
			duk_pop(ctx);

			duk_push_number(ctx, (ins & DUK_ENC_OP(0x02)) ? x : y);
			duk_replace(ctx, (duk_idx_t) a);
			break;
		}

		case DUK_OP_EXTRA: {
			/* XXX: shared decoding of 'b' and 'c'? */

			duk_small_uint_fast_t extraop = DUK_DEC_A(ins);
			switch ((int) extraop) {
			/* XXX: switch cast? */

			case DUK_EXTRAOP_NOP: {
				/* nop */
				break;
			}

			case DUK_EXTRAOP_INVALID: {
				DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, "INVALID opcode (%ld)", (long) DUK_DEC_BC(ins));
				break;
			}

			case DUK_EXTRAOP_LDTHIS: {
				/* Note: 'this' may be bound to any value, not just an object */
				duk_uint_fast_t bc = DUK_DEC_BC(ins);
				duk_tval tv_tmp;
				duk_tval *tv1, *tv2;

				tv1 = DUK__REGP(bc);
				tv2 = thr->valstack_bottom - 1;  /* 'this binding' is just under bottom */
				DUK_ASSERT(tv2 >= thr->valstack);

				DUK_DDD(DUK_DDDPRINT("LDTHIS: %!T to r%ld", (duk_tval *) tv2, (long) bc));

				DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
				DUK_TVAL_SET_TVAL(tv1, tv2);
				DUK_TVAL_INCREF(thr, tv1);
				DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
				break;
			}

			case DUK_EXTRAOP_LDUNDEF: {
				duk_uint_fast_t bc = DUK_DEC_BC(ins);
				duk_tval tv_tmp;
				duk_tval *tv1;

				tv1 = DUK__REGP(bc);
				DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
				DUK_TVAL_SET_UNDEFINED_ACTUAL(tv1);
				DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
				break;
			}

			case DUK_EXTRAOP_LDNULL: {
				duk_uint_fast_t bc = DUK_DEC_BC(ins);
				duk_tval tv_tmp;
				duk_tval *tv1;

				tv1 = DUK__REGP(bc);
				DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
				DUK_TVAL_SET_NULL(tv1);
				DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
				break;
			}

			case DUK_EXTRAOP_LDTRUE:
			case DUK_EXTRAOP_LDFALSE: {
				duk_uint_fast_t bc = DUK_DEC_BC(ins);
				duk_tval tv_tmp;
				duk_tval *tv1;
				duk_small_uint_fast_t bval = (extraop == DUK_EXTRAOP_LDTRUE ? 1 : 0);

				tv1 = DUK__REGP(bc);
				DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
				DUK_TVAL_SET_BOOLEAN(tv1, bval);
				DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
				break;
			}

			case DUK_EXTRAOP_NEWOBJ: {
				duk_context *ctx = (duk_context *) thr;
				duk_small_uint_fast_t b = DUK_DEC_B(ins);

				duk_push_object(ctx);
				duk_replace(ctx, (duk_idx_t) b);
				break;
			}

			case DUK_EXTRAOP_NEWARR: {
				duk_context *ctx = (duk_context *) thr;
				duk_small_uint_fast_t b = DUK_DEC_B(ins);

				duk_push_array(ctx);
				duk_replace(ctx, (duk_idx_t) b);
				break;
			}

			case DUK_EXTRAOP_SETALEN: {
				duk_small_uint_fast_t b;
				duk_small_uint_fast_t c;
				duk_tval *tv1;
				duk_hobject *h;
				duk_uint32_t len;

				b = DUK_DEC_B(ins); tv1 = DUK__REGP(b);
				DUK_ASSERT(DUK_TVAL_IS_OBJECT(tv1));
				h = DUK_TVAL_GET_OBJECT(tv1);

				c = DUK_DEC_C(ins); tv1 = DUK__REGP(c);
				DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv1));
				len = (duk_uint32_t) DUK_TVAL_GET_NUMBER(tv1);

				duk_hobject_set_length(thr, h, len);

				break;
			}

			case DUK_EXTRAOP_TYPEOF: {
				duk_context *ctx = (duk_context *) thr;
				duk_uint_fast_t bc = DUK_DEC_BC(ins);
				duk_push_hstring(ctx, duk_js_typeof(thr, DUK__REGP(bc)));
				duk_replace(ctx, (duk_idx_t) bc);
				break;
			}

			case DUK_EXTRAOP_TYPEOFID: {
				duk_context *ctx = (duk_context *) thr;
				duk_small_uint_fast_t b = DUK_DEC_B(ins);
				duk_small_uint_fast_t c = DUK_DEC_C(ins);
				duk_hstring *name;
				duk_tval *tv;

				/* B -> target register
				 * C -> constant index of identifier name
				 */

				tv = DUK__REGCONSTP(c);  /* XXX: this could be a DUK__CONSTP instead */
				DUK_ASSERT(DUK_TVAL_IS_STRING(tv));
				name = DUK_TVAL_GET_STRING(tv);
				if (duk_js_getvar_activation(thr, act, name, 0 /*throw*/)) {
					/* -> [... val this] */
					tv = duk_get_tval(ctx, -2);
					duk_push_hstring(ctx, duk_js_typeof(thr, tv));
					duk_replace(ctx, (duk_idx_t) b);
					duk_pop_2(ctx);
				} else {
					/* unresolvable, no stack changes */
					duk_push_hstring_stridx(ctx, DUK_STRIDX_LC_UNDEFINED);
					duk_replace(ctx, (duk_idx_t) b);
				}

				break;
			}

			case DUK_EXTRAOP_INITENUM: {
				duk_context *ctx = (duk_context *) thr;
				duk_small_uint_fast_t b = DUK_DEC_B(ins);
				duk_small_uint_fast_t c = DUK_DEC_C(ins);

				/*
				 *  Enumeration semantics come from for-in statement, E5 Section 12.6.4.
				 *  If called with 'null' or 'undefined', this opcode returns 'null' as
				 *  the enumerator, which is special cased in NEXTENUM.  This simplifies
				 *  the compiler part
				 */

				/* B -> register for writing enumerator object
				 * C -> value to be enumerated (register)
				 */

				if (duk_is_null_or_undefined(ctx, (duk_idx_t) c)) {
					duk_push_null(ctx);
					duk_replace(ctx, (duk_idx_t) b);
				} else {
					duk_dup(ctx, (duk_idx_t) c);
					duk_to_object(ctx, -1);
					duk_hobject_enumerator_create(ctx, 0 /*enum_flags*/);  /* [ ... val ] --> [ ... enum ] */
					duk_replace(ctx, (duk_idx_t) b);
				}
				break;
			}

			case DUK_EXTRAOP_NEXTENUM: {
				duk_context *ctx = (duk_context *) thr;
				duk_small_uint_fast_t b = DUK_DEC_B(ins);
				duk_small_uint_fast_t c = DUK_DEC_C(ins);

				/*
				 *  NEXTENUM checks whether the enumerator still has unenumerated
				 *  keys.  If so, the next key is loaded to the target register
				 *  and the next instruction is skipped.  Otherwise the next instruction
				 *  will be executed, jumping out of the enumeration loop.
				 */

				/* B -> target register for next key
				 * C -> enum register
				 */

				DUK_DDD(DUK_DDDPRINT("NEXTENUM: b->%!T, c->%!T",
				                     (duk_tval *) duk_get_tval(ctx, (duk_idx_t) b),
				                     (duk_tval *) duk_get_tval(ctx, (duk_idx_t) c)));

				if (duk_is_object(ctx, (duk_idx_t) c)) {
					/* XXX: assert 'c' is an enumerator */
					duk_dup(ctx, (duk_idx_t) c);
					if (duk_hobject_enumerator_next(ctx, 0 /*get_value*/)) {
						/* [ ... enum ] -> [ ... next_key ] */
						DUK_DDD(DUK_DDDPRINT("enum active, next key is %!T, skip jump slot ",
						                     (duk_tval *) duk_get_tval(ctx, -1)));
						act->pc++;;
					} else {
						/* [ ... enum ] -> [ ... ] */
						DUK_DDD(DUK_DDDPRINT("enum finished, execute jump slot"));
						duk_push_undefined(ctx);
					}
					duk_replace(ctx, (duk_idx_t) b);
				} else {
					/* 'null' enumerator case -> behave as with an empty enumerator */
					DUK_ASSERT(duk_is_null(ctx, (duk_idx_t) c));
					DUK_DDD(DUK_DDDPRINT("enum is null, execute jump slot"));
				}
				break;
			}

			case DUK_EXTRAOP_INITSET:
			case DUK_EXTRAOP_INITSETI:
			case DUK_EXTRAOP_INITGET:
			case DUK_EXTRAOP_INITGETI: {
				duk_context *ctx = (duk_context *) thr;
				duk_bool_t is_set = (extraop == DUK_EXTRAOP_INITSET || extraop == DUK_EXTRAOP_INITSETI);
				duk_small_uint_fast_t b = DUK_DEC_B(ins);
				duk_uint_fast_t idx;

				/* B -> object register
				 * C -> C+0 contains key, C+1 closure (value)
				 */

				/*
				 *  INITSET/INITGET are only used to initialize object literal keys.
				 *  The compiler ensures that there cannot be a previous data property
				 *  of the same name.  It also ensures that setter and getter can only
				 *  be initialized once (or not at all).
				 */

				idx = (duk_uint_fast_t) DUK_DEC_C(ins);
				if (extraop == DUK_EXTRAOP_INITSETI || extraop == DUK_EXTRAOP_INITGETI) {
					duk_tval *tv_ind = DUK__REGP(idx);
					DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_ind));
					idx = (duk_uint_fast_t) DUK_TVAL_GET_NUMBER(tv_ind);
				}

#if defined(DUK_USE_EXEC_INDIRECT_BOUND_CHECK)
				if (idx + 2 > (duk_uint_fast_t) duk_get_top(ctx)) {
					/* XXX: use duk_is_valid_index() instead? */
					/* XXX: improve check; check against nregs, not against top */
					DUK__INTERNAL_ERROR("INITSET/INITGET out of bounds");
				}
#endif

				/* XXX: this is now a very unoptimal implementation -- this can be
				 * made very simple by direct manipulation of the object internals,
				 * given the guarantees above.
				 */

				duk_push_hobject_bidx(ctx, DUK_BIDX_OBJECT_CONSTRUCTOR);
				duk_get_prop_stridx(ctx, -1, DUK_STRIDX_DEFINE_PROPERTY);
				duk_push_undefined(ctx);
				duk_dup(ctx, (duk_idx_t) b);
				duk_dup(ctx, (duk_idx_t) (idx + 0));
				duk_push_object(ctx);  /* -> [ Object defineProperty undefined obj key desc ] */

				duk_push_true(ctx);
				duk_put_prop_stridx(ctx, -2, DUK_STRIDX_ENUMERABLE);
				duk_push_true(ctx);
				duk_put_prop_stridx(ctx, -2, DUK_STRIDX_CONFIGURABLE);
				duk_dup(ctx, (duk_idx_t) (idx + 1));
				duk_put_prop_stridx(ctx, -2, (is_set ? DUK_STRIDX_SET : DUK_STRIDX_GET));

				DUK_DDD(DUK_DDDPRINT("INITGET/INITSET: obj=%!T, key=%!T, desc=%!T",
				                     (duk_tval *) duk_get_tval(ctx, -3),
				                     (duk_tval *) duk_get_tval(ctx, -2),
				                     (duk_tval *) duk_get_tval(ctx, -1)));

				duk_call_method(ctx, 3);  /* -> [ Object res ] */
				duk_pop_2(ctx);

				DUK_DDD(DUK_DDDPRINT("INITGET/INITSET AFTER: obj=%!T",
				                     (duk_tval *) duk_get_tval(ctx, (duk_idx_t) b)));
				break;
			}

			case DUK_EXTRAOP_ENDTRY: {
				duk_catcher *cat;
				duk_tval tv_tmp;
				duk_tval *tv1;

				DUK_ASSERT(thr->catchstack_top >= 1);
				DUK_ASSERT(thr->callstack_top >= 1);
				DUK_ASSERT(thr->catchstack[thr->catchstack_top - 1].callstack_index == thr->callstack_top - 1);

				cat = thr->catchstack + thr->catchstack_top - 1;

				DUK_DDD(DUK_DDDPRINT("ENDTRY: clearing catch active flag (regardless of whether it was set or not)"));
				DUK_CAT_CLEAR_CATCH_ENABLED(cat);

				if (DUK_CAT_HAS_FINALLY_ENABLED(cat)) {
					DUK_DDD(DUK_DDDPRINT("ENDTRY: finally part is active, jump through 2nd jump slot with 'normal continuation'"));

					tv1 = thr->valstack + cat->idx_base;
					DUK_ASSERT(tv1 >= thr->valstack && tv1 < thr->valstack_top);
					DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
					DUK_TVAL_SET_UNDEFINED_ACTUAL(tv1);
					DUK_TVAL_DECREF(thr, &tv_tmp);     /* side effects */
					tv1 = NULL;

					tv1 = thr->valstack + cat->idx_base + 1;
					DUK_ASSERT(tv1 >= thr->valstack && tv1 < thr->valstack_top);
					DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
					DUK_TVAL_SET_NUMBER(tv1, (duk_double_t) DUK_LJ_TYPE_NORMAL);  /* XXX: set int */
					DUK_TVAL_DECREF(thr, &tv_tmp);     /* side effects */
					tv1 = NULL;

					DUK_CAT_CLEAR_FINALLY_ENABLED(cat);
				} else {
					DUK_DDD(DUK_DDDPRINT("ENDTRY: no finally part, dismantle catcher, jump through 2nd jump slot (to end of statement)"));
					duk_hthread_catchstack_unwind(thr, thr->catchstack_top - 1);
					/* no need to unwind callstack */
				}

				act->pc = cat->pc_base + 1;
				break;
			}

			case DUK_EXTRAOP_ENDCATCH: {
				duk_catcher *cat;
				duk_tval tv_tmp;
				duk_tval *tv1;

				DUK_ASSERT(thr->catchstack_top >= 1);
				DUK_ASSERT(thr->callstack_top >= 1);
				DUK_ASSERT(thr->catchstack[thr->catchstack_top - 1].callstack_index == thr->callstack_top - 1);

				cat = thr->catchstack + thr->catchstack_top - 1;
				DUK_ASSERT(!DUK_CAT_HAS_CATCH_ENABLED(cat));  /* cleared before entering catch part */

				if (DUK_CAT_HAS_LEXENV_ACTIVE(cat)) {
					duk_hobject *prev_env;

					/* 'with' binding has no catch clause, so can't be here unless a normal try-catch */
					DUK_ASSERT(DUK_CAT_HAS_CATCH_BINDING_ENABLED(cat));
					DUK_ASSERT(act->lex_env != NULL);

					DUK_DDD(DUK_DDDPRINT("ENDCATCH: popping catcher part lexical environment"));

					prev_env = act->lex_env;
					DUK_ASSERT(prev_env != NULL);
					act->lex_env = DUK_HOBJECT_GET_PROTOTYPE(thr->heap, prev_env);
					DUK_CAT_CLEAR_LEXENV_ACTIVE(cat);
					DUK_HOBJECT_DECREF(thr, prev_env);  /* side effects */
				}

				if (DUK_CAT_HAS_FINALLY_ENABLED(cat)) {
					DUK_DDD(DUK_DDDPRINT("ENDCATCH: finally part is active, jump through 2nd jump slot with 'normal continuation'"));

					tv1 = thr->valstack + cat->idx_base;
					DUK_ASSERT(tv1 >= thr->valstack && tv1 < thr->valstack_top);
					DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
					DUK_TVAL_SET_UNDEFINED_ACTUAL(tv1);
					DUK_TVAL_DECREF(thr, &tv_tmp);     /* side effects */
					tv1 = NULL;

					tv1 = thr->valstack + cat->idx_base + 1;
					DUK_ASSERT(tv1 >= thr->valstack && tv1 < thr->valstack_top);
					DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
					DUK_TVAL_SET_NUMBER(tv1, (duk_double_t) DUK_LJ_TYPE_NORMAL);  /* XXX: set int */
					DUK_TVAL_DECREF(thr, &tv_tmp);     /* side effects */
					tv1 = NULL;

					DUK_CAT_CLEAR_FINALLY_ENABLED(cat);
				} else {
					DUK_DDD(DUK_DDDPRINT("ENDCATCH: no finally part, dismantle catcher, jump through 2nd jump slot (to end of statement)"));
					duk_hthread_catchstack_unwind(thr, thr->catchstack_top - 1);
					/* no need to unwind callstack */
				}

				act->pc = cat->pc_base + 1;
				break;
			}

			case DUK_EXTRAOP_ENDFIN: {
				duk_context *ctx = (duk_context *) thr;
				duk_catcher *cat;
				duk_tval *tv1;
				duk_small_uint_fast_t cont_type;

				DUK_ASSERT(thr->catchstack_top >= 1);
				DUK_ASSERT(thr->callstack_top >= 1);
				DUK_ASSERT(thr->catchstack[thr->catchstack_top - 1].callstack_index == thr->callstack_top - 1);

				cat = thr->catchstack + thr->catchstack_top - 1;

				/* CATCH flag may be enabled or disabled here; it may be enabled if
				 * the statement has a catch block but the try block does not throw
				 * an error.
				 */
				DUK_ASSERT(!DUK_CAT_HAS_FINALLY_ENABLED(cat));  /* cleared before entering finally */
				/* XXX: assert idx_base */

				DUK_DDD(DUK_DDDPRINT("ENDFIN: completion value=%!T, type=%!T",
				                     (duk_tval *) (thr->valstack + cat->idx_base + 0),
				                     (duk_tval *) (thr->valstack + cat->idx_base + 1)));

				tv1 = thr->valstack + cat->idx_base + 1;  /* type */
				DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv1));
				cont_type = (duk_small_uint_fast_t) DUK_TVAL_GET_NUMBER(tv1);

				if (cont_type == DUK_LJ_TYPE_NORMAL) {
					DUK_DDD(DUK_DDDPRINT("ENDFIN: finally part finishing with 'normal' (non-abrupt) completion -> "
					                     "dismantle catcher, resume execution after ENDFIN"));
					duk_hthread_catchstack_unwind(thr, thr->catchstack_top - 1);
					/* no need to unwind callstack */
				} else {
					DUK_DDD(DUK_DDDPRINT("ENDFIN: finally part finishing with abrupt completion, lj_type=%ld -> "
					                     "dismantle catcher, re-throw error",
					                     (long) cont_type));

					duk_push_tval(ctx, thr->valstack + cat->idx_base);

					/* XXX: assert lj type valid */
					duk_err_setup_heap_ljstate(thr, (duk_small_int_t) cont_type);

					DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);  /* always in executor */
					duk_err_longjmp(thr);
					DUK_UNREACHABLE();
				}

				/* continue execution after ENDFIN */
				break;
			}

			case DUK_EXTRAOP_THROW: {
				duk_context *ctx = (duk_context *) thr;
				duk_uint_fast_t bc = DUK_DEC_BC(ins);

				/* Note: errors are augmented when they are created, not
				 * when they are thrown.  So, don't augment here, it would
				 * break re-throwing for instance.
				 */

				duk_dup(ctx, (duk_idx_t) bc);
				DUK_DDD(DUK_DDDPRINT("THROW ERROR (BYTECODE): %!dT (before throw augment)",
				                     (duk_tval *) duk_get_tval(ctx, -1)));
#if defined(DUK_USE_AUGMENT_ERROR_THROW)
				duk_err_augment_error_throw(thr);
				DUK_DDD(DUK_DDDPRINT("THROW ERROR (BYTECODE): %!dT (after throw augment)",
				                     (duk_tval *) duk_get_tval(ctx, -1)));
#endif

				duk_err_setup_heap_ljstate(thr, DUK_LJ_TYPE_THROW);

				DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);  /* always in executor */
				duk_err_longjmp(thr);

				DUK_UNREACHABLE();
				break;
			}

			case DUK_EXTRAOP_INVLHS: {
				DUK_ERROR(thr, DUK_ERR_REFERENCE_ERROR, "invalid lvalue");

				DUK_UNREACHABLE();
				break;
			}

			case DUK_EXTRAOP_UNM:
			case DUK_EXTRAOP_UNP: {
				duk_uint_fast_t bc = DUK_DEC_BC(ins);
				duk__vm_arith_unary_op(thr, DUK__REGP(bc), bc, extraop);
				break;
			}

			case DUK_EXTRAOP_DEBUGGER: {
				/* Opcode only emitted by compiler when debugger
				 * support is enabled.  Ignore it silently without
				 * debugger support, in case it has been loaded
				 * from precompiled bytecode.
				 */
#if defined(DUK_USE_DEBUGGER_SUPPORT)
				DUK_D(DUK_DPRINT("DEBUGGER statement encountered, halt execution"));
				if (DUK_HEAP_IS_DEBUGGER_ATTACHED(thr->heap)) {
					DUK_HEAP_SET_PAUSED(thr->heap);
					goto restart_execution;
				}
#else
				DUK_D(DUK_DPRINT("DEBUGGER statement ignored, no debugger support"));
#endif
				break;
			}

			case DUK_EXTRAOP_BREAK: {
				duk_context *ctx = (duk_context *) thr;
				duk_uint_fast_t bc = DUK_DEC_BC(ins);

				/* always the "slow break" variant (longjmp'ing); a "fast break" is
				 * simply an DUK_OP_JUMP.
				 */

				DUK_DDD(DUK_DDDPRINT("BREAK: %ld", (long) bc));

				duk_push_uint(ctx, (duk_uint_t) bc);
				duk_err_setup_heap_ljstate(thr, DUK_LJ_TYPE_BREAK);

				DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);  /* always in executor */
				duk_err_longjmp(thr);

				DUK_UNREACHABLE();
				break;
			}

			case DUK_EXTRAOP_CONTINUE: {
				duk_context *ctx = (duk_context *) thr;
				duk_uint_fast_t bc = DUK_DEC_BC(ins);

				/* always the "slow continue" variant (longjmp'ing); a "fast continue" is
				 * simply an DUK_OP_JUMP.
				 */

				DUK_DDD(DUK_DDDPRINT("CONTINUE: %ld", (long) bc));

				duk_push_uint(ctx, (duk_uint_t) bc);
				duk_err_setup_heap_ljstate(thr, DUK_LJ_TYPE_CONTINUE);

				DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);  /* always in executor */
				duk_err_longjmp(thr);

				DUK_UNREACHABLE();
				break;
			}

			case DUK_EXTRAOP_BNOT: {
				duk_uint_fast_t bc = DUK_DEC_BC(ins);

				duk__vm_bitwise_not(thr, DUK__REGP(bc), bc);
				break;
			}

			case DUK_EXTRAOP_LNOT: {
				duk_uint_fast_t bc = DUK_DEC_BC(ins);
				duk_tval *tv1;

				tv1 = DUK__REGP(bc);
				duk__vm_logical_not(thr, tv1, tv1);
				break;
			}

			case DUK_EXTRAOP_INSTOF: {
				duk_context *ctx = (duk_context *) thr;
				duk_small_uint_fast_t b = DUK_DEC_B(ins);
				duk_small_uint_fast_t c = DUK_DEC_C(ins);
				duk_bool_t tmp;

				tmp = duk_js_instanceof(thr, DUK__REGP(b), DUK__REGCONSTP(c));
				duk_push_boolean(ctx, tmp);
				duk_replace(ctx, (duk_idx_t) b);
				break;
			}

			case DUK_EXTRAOP_IN: {
				duk_context *ctx = (duk_context *) thr;
				duk_small_uint_fast_t b = DUK_DEC_B(ins);
				duk_small_uint_fast_t c = DUK_DEC_C(ins);
				duk_bool_t tmp;

				tmp = duk_js_in(thr, DUK__REGP(b), DUK__REGCONSTP(c));
				duk_push_boolean(ctx, tmp);
				duk_replace(ctx, (duk_idx_t) b);
				break;
			}

			case DUK_EXTRAOP_LABEL: {
				duk_catcher *cat;
				duk_uint_fast_t bc = DUK_DEC_BC(ins);

				/* allocate catcher and populate it (should be atomic) */

				duk_hthread_catchstack_grow(thr);
				cat = thr->catchstack + thr->catchstack_top;
				thr->catchstack_top++;

				cat->flags = DUK_CAT_TYPE_LABEL | (bc << DUK_CAT_LABEL_SHIFT);
				cat->callstack_index = thr->callstack_top - 1;
				cat->pc_base = act->pc;  /* pre-incremented, points to first jump slot */
				cat->idx_base = 0;  /* unused for label */
				cat->h_varname = NULL;

				DUK_DDD(DUK_DDDPRINT("LABEL catcher: flags=0x%08lx, callstack_index=%ld, pc_base=%ld, "
				                     "idx_base=%ld, h_varname=%!O, label_id=%ld",
				                     (long) cat->flags, (long) cat->callstack_index, (long) cat->pc_base,
				                     (long) cat->idx_base, (duk_heaphdr *) cat->h_varname, (long) DUK_CAT_GET_LABEL(cat)));

				act->pc += 2;  /* skip jump slots */
				break;
			}

			case DUK_EXTRAOP_ENDLABEL: {
				duk_catcher *cat;
#if defined(DUK_USE_DDDPRINT) || defined(DUK_USE_ASSERTIONS)
				duk_uint_fast_t bc = DUK_DEC_BC(ins);
#endif
#if defined(DUK_USE_DDDPRINT)
				DUK_DDD(DUK_DDDPRINT("ENDLABEL %ld", (long) bc));
#endif

				DUK_ASSERT(thr->catchstack_top >= 1);

				cat = thr->catchstack + thr->catchstack_top - 1;
				DUK_UNREF(cat);
				DUK_ASSERT(DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_LABEL);
				DUK_ASSERT((duk_uint_fast_t) DUK_CAT_GET_LABEL(cat) == bc);

				duk_hthread_catchstack_unwind(thr, thr->catchstack_top - 1);
				/* no need to unwind callstack */
				break;
			}

#ifdef DUK_USE_DEBUG
			case DUK_EXTRAOP_DUMPREG: {
				DUK_D(DUK_DPRINT("DUMPREG: %ld -> %!T",
				                 (long) DUK_DEC_BC(ins),
				                 (duk_tval *) duk_get_tval((duk_context *) thr, (duk_idx_t) DUK_DEC_BC(ins))));
				break;
			}

			case DUK_EXTRAOP_DUMPREGS: {
				duk_idx_t i, i_top;
				i_top = duk_get_top((duk_context *) thr);
				DUK_D(DUK_DPRINT("DUMPREGS: %ld regs", (long) i_top));
				for (i = 0; i < i_top; i++) {
					DUK_D(DUK_DPRINT("  r%ld -> %!dT",
					                 (long) i,
					                 (duk_tval *) duk_get_tval((duk_context *) thr, i)));
				}
				break;
			}

			case DUK_EXTRAOP_LOGMARK: {
				DUK_D(DUK_DPRINT("LOGMARK: mark %ld at pc %ld", (long) DUK_DEC_BC(ins), (long) (act->pc - 1)));  /* -1, autoinc */
				break;
			}
#endif  /* DUK_USE_DEBUG */

			default: {
				DUK__INTERNAL_ERROR("invalid extra opcode");
			}

			}  /* end switch */

			break;
		}

		default: {
			/* this should never be possible, because the switch-case is
			 * comprehensive
			 */
			DUK__INTERNAL_ERROR("invalid opcode");
			break;
		}

		}  /* end switch */
	}
	DUK_UNREACHABLE();

#ifndef DUK_USE_VERBOSE_EXECUTOR_ERRORS
 internal_error:
	DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, "internal error in bytecode executor");
#endif
}

#undef DUK__INTERNAL_ERROR
