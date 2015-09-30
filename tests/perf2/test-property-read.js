/*
 *  Basic property read performance.
 */

if (typeof window == 'undefined') window = {}
if (!window.performance) window.performance = {}
window.performance.now = window.performance.now || Date.now

if (typeof print != 'undefined') log = print
else log = console.log

function test () {
  var start
  var duration

  var obj = { xxx1: 1, xxx2: 2, xxx3: 3, xxx4: 4, foo: 123 }
  var i, ign

  start = window.performance.now()

  // begin test
  for (i = 0; i < 1e8; i++) {
    ign = obj.foo
  }
  // end test

  duration = window.performance.now() - start

  log(duration)
}

test()
