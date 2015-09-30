/*
 *  Build string with concatenation.
 */

if (typeof window == 'undefined') window = {}
if (!window.performance) window.performance = {}
window.performance.now = window.performance.now || Date.now

if (typeof print != 'undefined') log = print
else log = console.log

function test () {
  var start
  var duration

  var i, j, t

  start = window.performance.now()

  // begin test
  for (i = 0; i < 5e3; i++) {
    t = []
    for (j = 0; j < 1e4; j++) {
      t[j] = 'x'
    }
    t = t.join('')
  }
  // end test

  duration = window.performance.now() - start

  log(duration)
}

test()
