/*
 *  Bytecode dump/load
 *
 *  The bytecode load primitive is more important performance-wise than the
 *  dump primitive.
 *
 *  Unlike most Duktape API calls, bytecode dump/load is not guaranteed to be
 *  memory safe for invalid arguments - caller beware!  There's little point
 *  in trying to achieve memory safety unless bytecode instructions are also
 *  validated which is not easy to do with indirect register references etc.
 */

#include "duk_internal.h"

#if defined(DUK_USE_BYTECODE_DUMP_SUPPORT)

#define DUK__SER_MARKER  0xff
#define DUK__SER_VERSION 0x00
#define DUK__SER_STRING  0x00
#define DUK__SER_NUMBER  0x01
#define DUK__BYTECODE_INITIAL_ALLOC 256

/*
 *  Dump/load helpers, xxx_raw() helpers do no buffer checks
 */

DUK_LOCAL duk_uint8_t *duk__load_string_raw(duk_context *ctx, duk_uint8_t *p) {
	duk_uint32_t len;

	len = DUK_RAW_READ_U32_BE(p);
	duk_push_lstring(ctx, (const char *) p, len);
	p += len;
	return p;
}

DUK_LOCAL duk_uint8_t *duk__load_buffer_raw(duk_context *ctx, duk_uint8_t *p) {
	duk_uint32_t len;
	duk_uint8_t *buf;

	len = DUK_RAW_READ_U32_BE(p);
	buf = (duk_uint8_t *) duk_push_fixed_buffer(ctx, (duk_size_t) len);
	DUK_ASSERT(buf != NULL);
	DUK_MEMCPY((void *) buf, (const void *) p, (size_t) len);
	p += len;
	return p;
}

DUK_LOCAL duk_uint8_t *duk__dump_hstring_raw(duk_uint8_t *p, duk_hstring *h) {
	duk_size_t len;
	duk_uint32_t tmp32;

	DUK_ASSERT(h != NULL);

	len = DUK_HSTRING_GET_BYTELEN(h);
	DUK_ASSERT(len <= 0xffffffffUL);  /* string limits */
	tmp32 = (duk_uint32_t) len;
	DUK_RAW_WRITE_U32_BE(p, tmp32);
	DUK_MEMCPY((void *) p,
	           (const void *) DUK_HSTRING_GET_DATA(h),
	           len);
	p += len;
	return p;
}

DUK_LOCAL duk_uint8_t *duk__dump_hbuffer_raw(duk_hthread *thr, duk_uint8_t *p, duk_hbuffer *h) {
	duk_size_t len;
	duk_uint32_t tmp32;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(h != NULL);
	DUK_UNREF(thr);

	len = DUK_HBUFFER_GET_SIZE(h);
	DUK_ASSERT(len <= 0xffffffffUL);  /* buffer limits */
	tmp32 = (duk_uint32_t) len;
	DUK_RAW_WRITE_U32_BE(p, tmp32);
	DUK_MEMCPY((void *) p,
	           (const void *) DUK_HBUFFER_GET_DATA_PTR(thr->heap, h),
	           len);
	p += len;
	return p;
}

DUK_LOCAL duk_uint8_t *duk__dump_string_prop(duk_hthread *thr, duk_uint8_t *p, duk_bufwriter_ctx *bw_ctx, duk_hobject *func, duk_small_uint_t stridx) {
	duk_hstring *h_str;
	duk_tval *tv;

	tv = duk_hobject_find_existing_entry_tval_ptr(thr->heap, (duk_hobject *) func, DUK_HTHREAD_GET_STRING(thr, stridx));
	if (tv != NULL && DUK_TVAL_IS_STRING(tv)) {
		h_str = DUK_TVAL_GET_STRING(tv);
		DUK_ASSERT(h_str != NULL);
	} else {
		h_str = DUK_HTHREAD_STRING_EMPTY_STRING(thr);
		DUK_ASSERT(h_str != NULL);
	}
	DUK_ASSERT(DUK_HSTRING_MAX_BYTELEN <= 0x7fffffffUL);  /* ensures no overflow */
	p = DUK_BW_ENSURE_RAW(thr, bw_ctx, 4 + DUK_HSTRING_GET_BYTELEN(h_str), p);
	p = duk__dump_hstring_raw(p, h_str);
	return p;
}

DUK_LOCAL duk_uint8_t *duk__dump_buffer_prop(duk_hthread *thr, duk_uint8_t *p, duk_bufwriter_ctx *bw_ctx, duk_hobject *func, duk_small_uint_t stridx) {
	duk_tval *tv;

	tv = duk_hobject_find_existing_entry_tval_ptr(thr->heap, (duk_hobject *) func, DUK_HTHREAD_GET_STRING(thr, stridx));
	if (tv != NULL && DUK_TVAL_IS_BUFFER(tv)) {
		duk_hbuffer *h_buf;
		h_buf = DUK_TVAL_GET_BUFFER(tv);
		DUK_ASSERT(h_buf != NULL);
		DUK_ASSERT(DUK_HBUFFER_MAX_BYTELEN <= 0x7fffffffUL);  /* ensures no overflow */
		p = DUK_BW_ENSURE_RAW(thr, bw_ctx, 4 + DUK_HBUFFER_GET_SIZE(h_buf), p);
		p = duk__dump_hbuffer_raw(thr, p, h_buf);
	} else {
		p = DUK_BW_ENSURE_RAW(thr, bw_ctx, 4, p);
		DUK_RAW_WRITE_U32_BE(p, 0);
	}
	return p;
}

DUK_LOCAL duk_uint8_t *duk__dump_uint32_prop(duk_hthread *thr, duk_uint8_t *p, duk_bufwriter_ctx *bw_ctx, duk_hobject *func, duk_small_uint_t stridx, duk_uint32_t def_value) {
	duk_tval *tv;
	duk_uint32_t val;

	tv = duk_hobject_find_existing_entry_tval_ptr(thr->heap, (duk_hobject *) func, DUK_HTHREAD_GET_STRING(thr, stridx));
	if (tv != NULL && DUK_TVAL_IS_NUMBER(tv)) {
		val = (duk_uint32_t) DUK_TVAL_GET_NUMBER(tv);
	} else {
		val = def_value;
	}
	p = DUK_BW_ENSURE_RAW(thr, bw_ctx, 4, p);
	DUK_RAW_WRITE_U32_BE(p, val);
	return p;
}

DUK_LOCAL duk_uint8_t *duk__dump_varmap(duk_hthread *thr, duk_uint8_t *p, duk_bufwriter_ctx *bw_ctx, duk_hobject *func) {
	duk_tval *tv;

	tv = duk_hobject_find_existing_entry_tval_ptr(thr->heap, (duk_hobject *) func, DUK_HTHREAD_STRING_INT_VARMAP(thr));
	if (tv != NULL && DUK_TVAL_IS_OBJECT(tv)) {
		duk_hobject *h;
		duk_uint_fast32_t i;

		h = DUK_TVAL_GET_OBJECT(tv);
		DUK_ASSERT(h != NULL);

		/* We know _Varmap only has own properties so walk property
		 * table directly.  We also know _Varmap is dense and all
		 * values are numbers; assert for these.  GC and finalizers
		 * shouldn't affect _Varmap so side effects should be fine.
		 */
		for (i = 0; i < (duk_uint_fast32_t) DUK_HOBJECT_GET_ENEXT(h); i++) {
			duk_hstring *key;
			duk_tval *tv_val;
			duk_uint32_t val;

			key = DUK_HOBJECT_E_GET_KEY(thr->heap, h, i);
			DUK_ASSERT(key != NULL);  /* _Varmap is dense */
			DUK_ASSERT(!DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, h, i));
			tv_val = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, h, i);
			DUK_ASSERT(tv_val != NULL);
			DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_val));  /* known to be number; in fact an integer */
#if defined(DUK_USE_FASTINT)
			DUK_ASSERT(DUK_TVAL_IS_FASTINT(tv_val));
			DUK_ASSERT(DUK_TVAL_GET_FASTINT(tv_val) == (duk_int64_t) DUK_TVAL_GET_FASTINT_U32(tv_val));  /* known to be 32-bit */
			val = DUK_TVAL_GET_FASTINT_U32(tv_val);
#else
			val = (duk_uint32_t) DUK_TVAL_GET_NUMBER(tv_val);
#endif

			DUK_ASSERT(DUK_HSTRING_MAX_BYTELEN <= 0x7fffffffUL);  /* ensures no overflow */
			p = DUK_BW_ENSURE_RAW(thr, bw_ctx, 4 + DUK_HSTRING_GET_BYTELEN(key) + 4, p);
			p = duk__dump_hstring_raw(p, key);
			DUK_RAW_WRITE_U32_BE(p, val);
		}
	}
	p = DUK_BW_ENSURE_RAW(thr, bw_ctx, 4, p);
	DUK_RAW_WRITE_U32_BE(p, 0);  /* end of _Varmap */
	return p;
}

DUK_LOCAL duk_uint8_t *duk__dump_formals(duk_hthread *thr, duk_uint8_t *p, duk_bufwriter_ctx *bw_ctx, duk_hobject *func) {
	duk_tval *tv;

	tv = duk_hobject_find_existing_entry_tval_ptr(thr->heap, (duk_hobject *) func, DUK_HTHREAD_STRING_INT_FORMALS(thr));
	if (tv != NULL && DUK_TVAL_IS_OBJECT(tv)) {
		duk_hobject *h;
		duk_uint_fast32_t i;

		h = DUK_TVAL_GET_OBJECT(tv);
		DUK_ASSERT(h != NULL);

		/* We know _Formals is dense and all entries will be in the
		 * array part.  GC and finalizers shouldn't affect _Formals
		 * so side effects should be fine.
		 */
		for (i = 0; i < (duk_uint_fast32_t) DUK_HOBJECT_GET_ASIZE(h); i++) {
			duk_tval *tv_val;
			duk_hstring *varname;

			tv_val = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, h, i);
			DUK_ASSERT(tv_val != NULL);
			if (DUK_TVAL_IS_STRING(tv_val)) {
				/* Array is dense and contains only strings, but ASIZE may
				 * be larger than used part and there are UNUSED entries.
				 */
				varname = DUK_TVAL_GET_STRING(tv_val);
				DUK_ASSERT(varname != NULL);

				DUK_ASSERT(DUK_HSTRING_MAX_BYTELEN <= 0x7fffffffUL);  /* ensures no overflow */
				p = DUK_BW_ENSURE_RAW(thr, bw_ctx, 4 + DUK_HSTRING_GET_BYTELEN(varname), p);
				p = duk__dump_hstring_raw(p, varname);
			}
		}
	}
	p = DUK_BW_ENSURE_RAW(thr, bw_ctx, 4, p);
	DUK_RAW_WRITE_U32_BE(p, 0);  /* end of _Formals */
	return p;
}

static duk_uint8_t *duk__dump_func(duk_context *ctx, duk_hcompiledfunction *func, duk_bufwriter_ctx *bw_ctx, duk_uint8_t *p) {
	duk_hthread *thr;
	duk_tval *tv, *tv_end;
	duk_instr_t *ins, *ins_end;
	duk_hobject **fn, **fn_end;
	duk_hstring *h_str;
	duk_uint32_t count_instr;
	duk_uint32_t tmp32;
	duk_uint16_t tmp16;
	duk_double_t d;

	thr = (duk_hthread *) ctx;
	DUK_UNREF(ctx);
	DUK_UNREF(thr);

	DUK_DD(DUK_DDPRINT("dumping function %p to %p: "
	                   "consts=[%p,%p[ (%ld bytes, %ld items), "
	                   "funcs=[%p,%p[ (%ld bytes, %ld items), "
	                   "code=[%p,%p[ (%ld bytes, %ld items)",
	                   (void *) func,
	                   (void *) p,
	                   (void *) DUK_HCOMPILEDFUNCTION_GET_CONSTS_BASE(thr->heap, func),
	                   (void *) DUK_HCOMPILEDFUNCTION_GET_CONSTS_END(thr->heap, func),
	                   (long) DUK_HCOMPILEDFUNCTION_GET_CONSTS_SIZE(thr->heap, func),
	                   (long) DUK_HCOMPILEDFUNCTION_GET_CONSTS_COUNT(thr->heap, func),
	                   (void *) DUK_HCOMPILEDFUNCTION_GET_FUNCS_BASE(thr->heap, func),
	                   (void *) DUK_HCOMPILEDFUNCTION_GET_FUNCS_END(thr->heap, func),
	                   (long) DUK_HCOMPILEDFUNCTION_GET_FUNCS_SIZE(thr->heap, func),
	                   (long) DUK_HCOMPILEDFUNCTION_GET_FUNCS_COUNT(thr->heap, func),
	                   (void *) DUK_HCOMPILEDFUNCTION_GET_CODE_BASE(thr->heap, func),
	                   (void *) DUK_HCOMPILEDFUNCTION_GET_CODE_END(thr->heap, func),
	                   (long) DUK_HCOMPILEDFUNCTION_GET_CODE_SIZE(thr->heap, func),
	                   (long) DUK_HCOMPILEDFUNCTION_GET_CODE_COUNT(thr->heap, func)));

	DUK_ASSERT(DUK_USE_ESBC_MAX_BYTES <= 0x7fffffffUL);  /* ensures no overflow */
	count_instr = (duk_uint32_t) DUK_HCOMPILEDFUNCTION_GET_CODE_COUNT(thr->heap, func);
	p = DUK_BW_ENSURE_RAW(thr, bw_ctx, 3 * 4 + 2 * 2 + 3 * 4 + count_instr * 4, p);

	/* Fixed header info. */
	tmp32 = count_instr;
	DUK_RAW_WRITE_U32_BE(p, tmp32);
	tmp32 = (duk_uint32_t) DUK_HCOMPILEDFUNCTION_GET_CONSTS_COUNT(thr->heap, func);
	DUK_RAW_WRITE_U32_BE(p, tmp32);
	tmp32 = (duk_uint32_t) DUK_HCOMPILEDFUNCTION_GET_FUNCS_COUNT(thr->heap, func);
	DUK_RAW_WRITE_U32_BE(p, tmp32);
	tmp16 = func->nregs;
	DUK_RAW_WRITE_U16_BE(p, tmp16);
	tmp16 = func->nargs;
	DUK_RAW_WRITE_U16_BE(p, tmp16);
#if defined(DUK_USE_DEBUGGER_SUPPORT)
	tmp32 = func->start_line;
	DUK_RAW_WRITE_U32_BE(p, tmp32);
	tmp32 = func->end_line;
	DUK_RAW_WRITE_U32_BE(p, tmp32);
#else
	DUK_RAW_WRITE_U32_BE(p, 0);
	DUK_RAW_WRITE_U32_BE(p, 0);
#endif
	tmp32 = ((duk_heaphdr *) func)->h_flags & DUK_HEAPHDR_FLAGS_FLAG_MASK;
	DUK_RAW_WRITE_U32_BE(p, tmp32);

	/* Bytecode instructions: endian conversion needed unless
	 * platform is big endian.
	 */
	ins = DUK_HCOMPILEDFUNCTION_GET_CODE_BASE(thr->heap, func);
	ins_end = DUK_HCOMPILEDFUNCTION_GET_CODE_END(thr->heap, func);
	DUK_ASSERT((duk_size_t) (ins_end - ins) == (duk_size_t) count_instr);
#if defined(DUK_USE_INTEGER_BE)
	DUK_MEMCPY((void *) p, (const void *) ins, (size_t) (ins_end - ins));
	p += (size_t) (ins_end - ins);
#else
	while (ins != ins_end) {
		tmp32 = (duk_uint32_t) (*ins);
		DUK_RAW_WRITE_U32_BE(p, tmp32);
		ins++;
	}
#endif

	/* Constants: variable size encoding. */
	tv = DUK_HCOMPILEDFUNCTION_GET_CONSTS_BASE(thr->heap, func);
	tv_end = DUK_HCOMPILEDFUNCTION_GET_CONSTS_END(thr->heap, func);
	while (tv != tv_end) {
		/* constants are strings or numbers now */
		DUK_ASSERT(DUK_TVAL_IS_STRING(tv) ||
		           DUK_TVAL_IS_NUMBER(tv));

		if (DUK_TVAL_IS_STRING(tv)) {
			h_str = DUK_TVAL_GET_STRING(tv);
			DUK_ASSERT(h_str != NULL);
			DUK_ASSERT(DUK_HSTRING_MAX_BYTELEN <= 0x7fffffffUL);  /* ensures no overflow */
			p = DUK_BW_ENSURE_RAW(thr, bw_ctx, 1 + 4 + DUK_HSTRING_GET_BYTELEN(h_str), p),
			*p++ = DUK__SER_STRING;
			p = duk__dump_hstring_raw(p, h_str);
		} else {
			DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv));
			p = DUK_BW_ENSURE_RAW(thr, bw_ctx, 1 + 8, p);
			*p++ = DUK__SER_NUMBER;
			d = DUK_TVAL_GET_NUMBER(tv);
			DUK_RAW_WRITE_DOUBLE_BE(p, d);
		}
		tv++;
	}

	/* Inner functions recursively. */
	fn = (duk_hobject **) DUK_HCOMPILEDFUNCTION_GET_FUNCS_BASE(thr->heap, func);
	fn_end = (duk_hobject **) DUK_HCOMPILEDFUNCTION_GET_FUNCS_END(thr->heap, func);
	while (fn != fn_end) {
		/* XXX: This causes recursion up to inner function depth
		 * which is normally not an issue, e.g. mark-and-sweep uses
		 * a recursion limiter to avoid C stack issues.  Avoiding
		 * this would mean some sort of a work list or just refusing
		 * to serialize deep functions.
		 */
		DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(*fn));
		p = duk__dump_func(ctx, (duk_hcompiledfunction *) *fn, bw_ctx, p);
		fn++;
	}

	/* Object extra properties.
	 *
	 * There are some difference between function templates and functions.
	 * For example, function templates don't have .length and nargs is
	 * normally used to instantiate the functions.
	 */

	p = duk__dump_uint32_prop(thr, p, bw_ctx, (duk_hobject *) func, DUK_STRIDX_LENGTH, (duk_uint32_t) func->nargs);
	p = duk__dump_string_prop(thr, p, bw_ctx, (duk_hobject *) func, DUK_STRIDX_NAME);
	p = duk__dump_string_prop(thr, p, bw_ctx, (duk_hobject *) func, DUK_STRIDX_FILE_NAME);
	p = duk__dump_buffer_prop(thr, p, bw_ctx, (duk_hobject *) func, DUK_STRIDX_INT_PC2LINE);
	p = duk__dump_varmap(thr, p, bw_ctx, (duk_hobject *) func);
	p = duk__dump_formals(thr, p, bw_ctx, (duk_hobject *) func);

	DUK_DD(DUK_DDPRINT("serialized function %p -> final pointer %p", (void *) func, (void *) p));

	return p;
}

/* Load a function from bytecode.  The function object returned here must
 * match what is created by duk_js_push_closure() with respect to its flags,
 * properties, etc.
 *
 * NOTE: there are intentionally no input buffer length / bound checks.
 * Adding them would be easy but wouldn't ensure memory safety as untrusted
 * or broken bytecode is unsafe during execution unless the opcodes themselves
 * are validated (which is quite complex, especially for indirect opcodes).
 */

#define DUK__ASSERT_LEFT(n) do { \
		DUK_ASSERT((duk_size_t) (p_end - p) >= (duk_size_t) (n)); \
	} while (0)

static duk_uint8_t *duk__load_func(duk_context *ctx, duk_uint8_t *p, duk_uint8_t *p_end) {
	duk_hthread *thr;
	duk_hcompiledfunction *h_fun;
	duk_hbuffer *h_data;
	duk_size_t data_size;
	duk_uint32_t count_instr, count_const, count_funcs;
	duk_uint32_t n;
	duk_uint32_t tmp32;
	duk_small_uint_t const_type;
	duk_uint8_t *fun_data;
	duk_uint8_t *q;
	duk_idx_t idx_base;
	duk_tval *tv;
	duk_uarridx_t arr_idx;

	/* XXX: There's some overlap with duk_js_closure() here, but
	 * seems difficult to share code.  Ensure that the final function
	 * looks the same as created by duk_js_closure().
	 */

	DUK_ASSERT(ctx != NULL);
	thr = (duk_hthread *) ctx;

	DUK_DD(DUK_DDPRINT("loading function, p=%p, p_end=%p", (void *) p, (void *) p_end));

	DUK__ASSERT_LEFT(3 * 4);
	count_instr = DUK_RAW_READ_U32_BE(p);
	count_const = DUK_RAW_READ_U32_BE(p);
	count_funcs = DUK_RAW_READ_U32_BE(p);

	data_size = sizeof(duk_tval) * count_const +
	            sizeof(duk_hobject *) * count_funcs +
	            sizeof(duk_instr_t) * count_instr;

	DUK_DD(DUK_DDPRINT("instr=%ld, const=%ld, funcs=%ld, data_size=%ld",
	                   (long) count_instr, (long) count_const,
	                   (long) count_const, (long) data_size));

	/* Value stack is used to ensure reachability of constants and
	 * inner functions being loaded.  Require enough space to handle
	 * large functions correctly.
	 */
	duk_require_stack(ctx, 2 + count_const + count_funcs);
	idx_base = duk_get_top(ctx);

	/* Push function object, init flags etc.  This must match
	 * duk_js_push_closure() quite carefully.
	 */
	duk_push_compiledfunction(ctx);
	h_fun = duk_get_hcompiledfunction(ctx, -1);
	DUK_ASSERT(h_fun != NULL);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION((duk_hobject *) h_fun));
	DUK_ASSERT(DUK_HCOMPILEDFUNCTION_GET_DATA(thr->heap, h_fun) == NULL);
	DUK_ASSERT(DUK_HCOMPILEDFUNCTION_GET_FUNCS(thr->heap, h_fun) == NULL);
	DUK_ASSERT(DUK_HCOMPILEDFUNCTION_GET_BYTECODE(thr->heap, h_fun) == NULL);

	h_fun->nregs = DUK_RAW_READ_U16_BE(p);
	h_fun->nargs = DUK_RAW_READ_U16_BE(p);
#if defined(DUK_USE_DEBUGGER_SUPPORT)
	h_fun->start_line = DUK_RAW_READ_U32_BE(p);
	h_fun->end_line = DUK_RAW_READ_U32_BE(p);
#else
	p += 8;  /* skip line info */
#endif

	/* duk_hcompiledfunction flags; quite version specific */
	tmp32 = DUK_RAW_READ_U32_BE(p);
	DUK_HEAPHDR_SET_FLAGS((duk_heaphdr *) h_fun, tmp32);

	/* standard prototype */
	DUK_HOBJECT_SET_PROTOTYPE_UPDREF(thr, &h_fun->obj, thr->builtins[DUK_BIDX_FUNCTION_PROTOTYPE]);

	/* assert just a few critical flags */
	DUK_ASSERT(DUK_HEAPHDR_GET_TYPE((duk_heaphdr *) h_fun) == DUK_HTYPE_OBJECT);
	DUK_ASSERT(!DUK_HOBJECT_HAS_BOUND(&h_fun->obj));
	DUK_ASSERT(DUK_HOBJECT_HAS_COMPILEDFUNCTION(&h_fun->obj));
	DUK_ASSERT(!DUK_HOBJECT_HAS_NATIVEFUNCTION(&h_fun->obj));
	DUK_ASSERT(!DUK_HOBJECT_HAS_THREAD(&h_fun->obj));
	DUK_ASSERT(!DUK_HOBJECT_HAS_EXOTIC_ARRAY(&h_fun->obj));
	DUK_ASSERT(!DUK_HOBJECT_HAS_EXOTIC_STRINGOBJ(&h_fun->obj));
	DUK_ASSERT(!DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(&h_fun->obj));

	/* Create function 'data' buffer but don't attach it yet. */
	fun_data = (duk_uint8_t *) duk_push_fixed_buffer(ctx, data_size);
	DUK_ASSERT(fun_data != NULL);

	/* Load bytecode instructions. */
	DUK_ASSERT(sizeof(duk_instr_t) == 4);
	DUK__ASSERT_LEFT(count_instr * sizeof(duk_instr_t));
#if defined(DUK_USE_INTEGER_BE)
	q = fun_data + sizeof(duk_tval) * count_const + sizeof(duk_hobject *) * count_funcs;
	DUK_MEMCPY((void *) q,
	           (const void *) p,
	           sizeof(duk_instr_t) * count_instr);
	p += sizeof(duk_instr_t) * count_instr;
#else
	q = fun_data + sizeof(duk_tval) * count_const + sizeof(duk_hobject *) * count_funcs;
	for (n = count_instr; n > 0; n--) {
		*((duk_instr_t *) q) = DUK_RAW_READ_U32_BE(p);
		q += sizeof(duk_instr_t);
	}
#endif

	/* Load constants onto value stack but don't yet copy to buffer. */
	for (n = count_const; n > 0; n--) {
		DUK__ASSERT_LEFT(1);
		const_type = DUK_RAW_READ_U8(p);
		switch (const_type) {
		case DUK__SER_STRING: {
			p = duk__load_string_raw(ctx, p);
			break;
		}
		case DUK__SER_NUMBER: {
			duk_double_t val;
			DUK__ASSERT_LEFT(8);
			val = DUK_RAW_READ_DOUBLE_BE(p);
			duk_push_number(ctx, val);
			break;
		}
		default: {
			goto format_error;
		}
		}
	}

	/* Load inner functions to value stack, but don't yet copy to buffer. */
	for (n = count_funcs; n > 0; n--) {
		p = duk__load_func(ctx, p, p_end);
		if (p == NULL) {
			goto format_error;
		}
	}

	/* With constants and inner functions on value stack, we can now
	 * atomically finish the function 'data' buffer, bump refcounts,
	 * etc.
	 *
	 * Here we take advantage of the value stack being just a duk_tval
	 * array: we can just memcpy() the constants as long as we incref
	 * them afterwards.
	 */

	h_data = (duk_hbuffer *) duk_get_hbuffer(ctx, idx_base + 1);
	DUK_ASSERT(h_data != NULL);
	DUK_ASSERT(!DUK_HBUFFER_HAS_DYNAMIC(h_data));
	DUK_HCOMPILEDFUNCTION_SET_DATA(thr->heap, h_fun, h_data);
	DUK_HBUFFER_INCREF(thr, h_data);

	tv = duk_get_tval(ctx, idx_base + 2);  /* may be NULL if no constants or inner funcs */
	DUK_ASSERT((count_const == 0 && count_funcs == 0) || tv != NULL);

	q = fun_data;
	if (count_const > 0) {
		/* Explicit zero size check to avoid NULL 'tv'. */
		DUK_MEMCPY((void *) q, (const void *) tv, sizeof(duk_tval) * count_const);
		for (n = count_const; n > 0; n--) {
			DUK_TVAL_INCREF_FAST(thr, (duk_tval *) q);  /* no side effects */
			q += sizeof(duk_tval);
		}
		tv += count_const;
	}

	DUK_HCOMPILEDFUNCTION_SET_FUNCS(thr->heap, h_fun, (duk_hobject **) q);
	for (n = count_funcs; n > 0; n--) {
		duk_hobject *h_obj;

		DUK_ASSERT(DUK_TVAL_IS_OBJECT(tv));
		h_obj = DUK_TVAL_GET_OBJECT(tv);
		DUK_ASSERT(h_obj != NULL);
		tv++;
		DUK_HOBJECT_INCREF(thr, h_obj);

		*((duk_hobject **) q) = h_obj;
		q += sizeof(duk_hobject *);
	}

	DUK_HCOMPILEDFUNCTION_SET_BYTECODE(thr->heap, h_fun, (duk_instr_t *) q);

	/* The function object is now reachable and refcounts are fine,
	 * so we can pop off all the temporaries.
	 */
	DUK_DDD(DUK_DDDPRINT("function is reachable, reset top; func: %!iT", duk_get_tval(ctx, idx_base)));
	duk_set_top(ctx, idx_base + 1);

	/* Setup function properties. */
	tmp32 = DUK_RAW_READ_U32_BE(p);
	duk_push_u32(ctx, tmp32);
	duk_xdef_prop_stridx(ctx, -2, DUK_STRIDX_LENGTH, DUK_PROPDESC_FLAGS_NONE);

	p = duk__load_string_raw(ctx, p);
	if (DUK_HOBJECT_HAS_NAMEBINDING((duk_hobject *) h_fun)) {
		/* Original function instance/template had NAMEBINDING.
		 * Must create a lexical environment on loading to allow
		 * recursive functions like 'function foo() { foo(); }'.
		 */
		duk_hobject *proto;

		proto = thr->builtins[DUK_BIDX_GLOBAL_ENV];
		(void) duk_push_object_helper_proto(ctx,
		                                    DUK_HOBJECT_FLAG_EXTENSIBLE |
		                                    DUK_HOBJECT_CLASS_AS_FLAGS(DUK_HOBJECT_CLASS_DECENV),
		                                    proto);
		duk_dup(ctx, -2);                                 /* -> [ func funcname env funcname ] */
		duk_dup(ctx, idx_base);                           /* -> [ func funcname env funcname func ] */
		duk_xdef_prop(ctx, -3, DUK_PROPDESC_FLAGS_NONE);  /* -> [ func funcname env ] */
		duk_xdef_prop_stridx(ctx, idx_base, DUK_STRIDX_INT_LEXENV, DUK_PROPDESC_FLAGS_WC);
		/* since closure has NEWENV, never define DUK_STRIDX_INT_VARENV, as it
		 * will be ignored anyway
		 */
	}
	duk_xdef_prop_stridx(ctx, -2, DUK_STRIDX_NAME, DUK_PROPDESC_FLAGS_NONE);

	p = duk__load_string_raw(ctx, p);
	duk_xdef_prop_stridx(ctx, -2, DUK_STRIDX_FILE_NAME, DUK_PROPDESC_FLAGS_WC);

	duk_push_object(ctx);
	duk_dup(ctx, -2);
	duk_xdef_prop_stridx(ctx, -2, DUK_STRIDX_CONSTRUCTOR, DUK_PROPDESC_FLAGS_WC);  /* func.prototype.constructor = func */
	duk_compact(ctx, -1);
	duk_xdef_prop_stridx(ctx, -2, DUK_STRIDX_PROTOTYPE, DUK_PROPDESC_FLAGS_W);

	p = duk__load_buffer_raw(ctx, p);
	duk_xdef_prop_stridx(ctx, -2, DUK_STRIDX_INT_PC2LINE, DUK_PROPDESC_FLAGS_WC);

	duk_push_object(ctx);  /* _Varmap */
	for (;;) {
		/* XXX: awkward */
		p = duk__load_string_raw(ctx, p);
		if (duk_get_length(ctx, -1) == 0) {
			duk_pop(ctx);
			break;
		}
		tmp32 = DUK_RAW_READ_U32_BE(p);
		duk_push_u32(ctx, tmp32);
		duk_put_prop(ctx, -3);
	}
	duk_compact(ctx, -1);
	duk_xdef_prop_stridx(ctx, -2, DUK_STRIDX_INT_VARMAP, DUK_PROPDESC_FLAGS_NONE);

	duk_push_array(ctx);  /* _Formals */
	for (arr_idx = 0; ; arr_idx++) {
		/* XXX: awkward */
		p = duk__load_string_raw(ctx, p);
		if (duk_get_length(ctx, -1) == 0) {
			duk_pop(ctx);
			break;
		}
		duk_put_prop_index(ctx, -2, arr_idx);
	}
	duk_compact(ctx, -1);
	duk_xdef_prop_stridx(ctx, -2, DUK_STRIDX_INT_FORMALS, DUK_PROPDESC_FLAGS_NONE);

	/* Return with final function pushed on stack top. */
	DUK_DD(DUK_DDPRINT("final loaded function: %!iT", duk_get_tval(ctx, -1)));
	DUK_ASSERT_TOP(ctx, idx_base + 1);
	return p;

 format_error:
	return NULL;
}

DUK_EXTERNAL void duk_dump_function(duk_context *ctx) {
	duk_hthread *thr;
	duk_hcompiledfunction *func;
	duk_bufwriter_ctx bw_ctx_alloc;
	duk_bufwriter_ctx *bw_ctx = &bw_ctx_alloc;
	duk_uint8_t *p;

	DUK_ASSERT(ctx != NULL);
	thr = (duk_hthread *) ctx;

	/* Bound functions don't have all properties so we'd either need to
	 * lookup the non-bound target function or reject bound functions.
	 * For now, bound functions are rejected.
	 */
	func = duk_require_hcompiledfunction(ctx, -1);
	DUK_ASSERT(func != NULL);
	DUK_ASSERT(!DUK_HOBJECT_HAS_BOUND(&func->obj));

	/* Estimating the result size beforehand would be costly, so
	 * start with a reasonable size and extend as needed.
	 */
	DUK_BW_INIT_PUSHBUF(thr, bw_ctx, DUK__BYTECODE_INITIAL_ALLOC);
	p = DUK_BW_GET_PTR(thr, bw_ctx);
	*p++ = DUK__SER_MARKER;
	*p++ = DUK__SER_VERSION;
	p = duk__dump_func(ctx, func, bw_ctx, p);
	DUK_BW_SET_PTR(thr, bw_ctx, p);
	DUK_BW_COMPACT(thr, bw_ctx);

	DUK_DD(DUK_DDPRINT("serialized result: %!T", duk_get_tval(ctx, -1)));

	duk_remove(ctx, -2);  /* [ ... func buf ] -> [ ... buf ] */
}

DUK_EXTERNAL void duk_load_function(duk_context *ctx) {
	duk_hthread *thr;
	duk_uint8_t *p_buf, *p, *p_end;
	duk_size_t sz;

	DUK_ASSERT(ctx != NULL);
	thr = (duk_hthread *) ctx;
	DUK_UNREF(ctx);

	p_buf = (duk_uint8_t *) duk_require_buffer(ctx, -1, &sz);
	DUK_ASSERT(p_buf != NULL);

	/* The caller is responsible for being sure that bytecode being loaded
	 * is valid and trusted.  Invalid bytecode can cause memory unsafe
	 * behavior directly during loading or later during bytecode execution
	 * (instruction validation would be quite complex to implement).
	 *
	 * This signature check is the only sanity check for detecting
	 * accidental invalid inputs.  The initial 0xFF byte ensures no
	 * ordinary string will be accepted by accident.
	 */
	p = p_buf;
	p_end = p_buf + sz;
	if (sz < 2 || p[0] != DUK__SER_MARKER || p[1] != DUK__SER_VERSION) {
		goto format_error;
	}
	p += 2;

	p = duk__load_func(ctx, p, p_end);
	if (p == NULL) {
		goto format_error;
	}

	duk_remove(ctx, -2);  /* [ ... buf func ] -> [ ... func ] */
	return;

 format_error:
	DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_DECODE_FAILED);
}

#undef DUK__SER_MARKER
#undef DUK__SER_VERSION
#undef DUK__SER_STRING
#undef DUK__SER_NUMBER
#undef DUK__BYTECODE_INITIAL_ALLOC

#else  /* DUK_USE_BYTECODE_DUMP_SUPPORT */

DUK_EXTERNAL void duk_dump_function(duk_context *ctx) {
	DUK_ERROR((duk_hthread *) ctx, DUK_ERR_ERROR, DUK_STR_UNSUPPORTED);
}

DUK_EXTERNAL void duk_load_function(duk_context *ctx) {
	DUK_ERROR((duk_hthread *) ctx, DUK_ERR_ERROR, DUK_STR_UNSUPPORTED);
}

#endif  /* DUK_USE_BYTECODE_DUMP_SUPPORT */
