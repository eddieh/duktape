/*
 *  Simple bitwise operation test.
 */

if (typeof window == 'undefined') window = {}
if (!window.performance) window.performance = {}
window.performance.now = window.performance.now || Date.now

if (typeof print != 'undefined') log = print
else log = console.log

function test () {
  var start
  var duration

  var x, y, z
  var i

  x = 0xdeadbeef
  y = 0xf

  start = window.performance.now()

  // begin test
  for (i = 0; i < 1e7; i++) {
    z = x & y;
    z = x | y;
    z = x ^ y;
    z = x << y;
    z = x >> y;
    z = x >>> y;
    z = ~x;
  }
  // end test

  duration = window.performance.now() - start

  log(duration)
}

test()
