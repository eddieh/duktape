/*
 *  Basic array read performance
 */

if (typeof window == 'undefined') window = {}
if (!window.performance) window.performance = {}
window.performance.now = window.performance.now || Date.now

if (typeof print != 'undefined') log = print
else log = console.log

function test () {
  var start, duration

  var i, ign
  var arr = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

  start = window.performance.now()

  // begin test
  for (i = 0; i < 1e8; i++) ign = arr[7]
  // end test

  duration = window.performance.now() - start

  log(duration)
}

test()
