/*
 *  JSON.stringify() fast path tests
 *
 *  Try to exercise all code paths in the fast path, and ensure that falling
 *  back to the slow path is transparent.
 */

/*===
basic test
{"foo":123,"bar":234,"quux":{"val2":null,"val3":true,"val4":false,"val5":123,"val6":123.456,"val7":"foo"},"baz":[null,null,true,false,123,123.456,"foo"]}
===*/

/* Fast path success case which should exercise all fast path code paths
 * if possible (but not aborting the fast path).
 */

function jsonStringifyFastPathBasic() {
    var val = {
        foo: 123,
        bar: 234,
        quux: {
            val1: undefined,
            val2: null,
            val3: true,
            val4: false,
            val5: 123,
            val6: 123.456,
            val7: 'foo'
        },
        baz: [
            undefined,
            null,
            true,
            false,
            123,
            123.456,
            'foo'
        ]
    };

    print(JSON.stringify(val));
}

try {
    print('basic test');
    jsonStringifyFastPathBasic();
} catch (e) {
    print(e.stack || e);
}

/*===
top level value test
0 undefined
1 null
2 true
3 false
4 123
5 123.456
6 "foo"
7 {"foo":123}
8 ["foo"]
9 undefined
10 "1970-01-01T00:00:00.123Z"
11 undefined
12 undefined
13 undefined
14 {"type":"Buffer","data":[65,66,67,68,69,70,71,72]}
===*/

/* Top level value */

function jsonStringifyFastPathTopLevelValueTest() {
    var values = [
        undefined, null, true, false, 123, 123.456, 'foo',
        { foo: 123 }, [ 'foo' ],
        function myfunc() {},
        new Date(123),
        Duktape.dec('hex', 'deadbeef'),
        new Duktape.Buffer(Duktape.dec('hex', 'deadbeef')),
        new ArrayBuffer(8),
        new Buffer('ABCDEFGH'),  // has toJSON
    ];

    values.forEach(function (v, i) {
        print(i, JSON.stringify(v));
    });
}

try {
    print('top level value test');
    jsonStringifyFastPathTopLevelValueTest();
} catch (e) {
    print(e.stack || e);
}

/*===
auto unbox test
0 123
1 "foo"
2 true
3 false
===*/

/* JSON requires automatic unboxing of the following primitive types:
 * Number, String, Boolean (E5 Section 15.12.3, Str() algorithm, step 4).
 */

function jsonStringifyFastPathAutoUnboxTest() {
    var values = [
        new Number(123),
        new String('foo'),
        new Boolean(true),
        new Boolean(false)
    ];

    values.forEach(function (v, i) {
        print(i, JSON.stringify(v));
    });
}

try {
    print('auto unbox test');
    jsonStringifyFastPathAutoUnboxTest();
} catch (e) {
    print(e.stack || e);
}

/*===
abort test
0 "foobar"
1 [null]
mygetter called
2 {}
3 [1,2,3,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,4]
4 {"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{"deeper":{}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}
no-inherit [1,2,3,4,null,null,6]
inherit [1,2,3,4,null,"inherit",6]
===*/

/* Fast path is based on enumerating object properties directly without using
 * an explicit enumerator.  However, the fast path must be aborted if there's
 * danger of a side effect which might lead to mutation of the value(s) being
 * serialized.
 *
 * A value replacer might have such side effects but it's not supported in the
 * fast path at all.
 *
 * Presence of a .toJSON() method is another reason; the fast path will now
 * detect this case and abort.
 *
 * There are also some technical ones like sparse arrays etc.
 */

function jsonStringifyFastPathAbort() {
    var values = [];
    var obj;
    var i;

    // a .toJSON property aborts
    values.push({ toJSON: function () { return 'foobar'; } });

    // a lightfunc value might inherit a .toJSON, so lightfuncs always
    // cause an abort
    values.push([ Math.cos ]);  // only a lightfunc if "built-in lightfuncs" option set

    // a getter property aborts
    obj = {};
    Object.defineProperty(obj, 'mygetter', {
        get: function () {
            print('mygetter called');
            obj.foo = 'bar';  // mutate, shouldn't be visible in output
        },
        enumerable: true,
        configurable: true
    });
    values.push(obj);

    // a sparse Array aborts fast path
    obj = [ 1, 2, 3 ];
    obj[100] = 4;
    values.push(obj);

    // a non-cyclic structure which is larger than the fast path loop check
    // array (which has a fixed size, currently 32 elements) should abort
    // the fast path and *succeed* in the slow path which has a much larger
    // recursion limit.
    var deep = {};
    for (i = 0; i < 100; i++) {
        deep = { deeper: deep };
    }
    values.push(deep);

    values.forEach(function (v, i) {
        print(i, JSON.stringify(v));
    });

    // a dense gappy Array with a conflicting inherited property causes a
    // bailout (needs separate test because we modify Array.prototype)

    obj = [ 1, 2, 3, 4 ];
    obj[6] = 6;  // still dense
    print('no-inherit', JSON.stringify(obj));  // fast path OK
    Object.defineProperty(Array.prototype, '5', {
        writable: true, enumerable: true, configurable: true,
        value: 'inherit'
    });
    print('inherit', JSON.stringify(obj));  // fast path aborted
    delete Array.prototype['5'];
}

try {
    print('abort test');
    jsonStringifyFastPathAbort();
} catch (e) {
    print(e.stack || e);
}

/*===
inheritance test
parent foo
child bar
{"bar":"child bar"}
["foo","bar","quux","baz"]
["foo",null,"quux","baz"]
["foo","parent-bar","quux","baz"]
===*/

/* Property inheritance is quite interesting, test for correct behavior. */

function jsonStringifyFastPathInheritanceTest() {
    var obj1, obj2;

    /* For objects JSON.stringify() only looks at enumerable own properties so
     * it's very simple.
     */

    obj1 = { foo: 'parent foo' };
    obj2 = Object.create(obj1);
    obj2.bar = 'child bar';
    print(obj2.foo);
    print(obj2.bar);
    print(JSON.stringify(obj2));

    /* For arrays JSON.stringify() uses the [[Get]] operation which -does-
     * inherit through array gaps.  Ensure that works correctly.
     */
    obj1 = [ 'foo', 'bar', 'quux', 'baz' ];
    print(JSON.stringify(obj1));

    // create gap, nothing inherited; this is now supported in the fast path
    delete obj1[1];
    print(JSON.stringify(obj1));

    // inherit something through the gap; this now bails out of the fast path
    Array.prototype[1] = 'parent-bar';
    print(JSON.stringify(obj1));

    delete Array.prototype[1];  // restore sanity
}

try {
    print('inheritance test');
    jsonStringifyFastPathInheritanceTest();
} catch (e) {
    print(e.stack || e);
}
