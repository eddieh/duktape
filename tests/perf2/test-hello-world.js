/*
 *  Obligatory test.
 */

if (typeof window == 'undefined') window = {}
if (!window.performance) window.performance = {}
window.performance.now = window.performance.now || Date.now

if (typeof print != 'undefined') log = print
else log = console.log

function test () {
  var start
  var duration

  start = window.performance.now()

  // begin test
  log('Hello, world!')
  // end test

  duration = window.performance.now() - start

  log(duration)
}

test()
