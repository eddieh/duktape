function test() {
    var b = new Duktape.Buffer(4096);
    var i;

    print(typeof b);

    for (i = 0; i < 1e8; i++) {
        void b[100];
    }
}

try {
    test();
} catch (e) {
    print(e.stack || e);
    throw e;
}
