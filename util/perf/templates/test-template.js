/*
 *  {{TEST_DESCRIPTION}}
 */

if (typeof window == 'undefined') window = {}
if (!window.performance) window.performance = {}
window.performance.now = window.performance.now || Date.now

if (typeof print != 'undefined') log = print
else log = console.log

function test () {
  var start
  var duration

  // setup test

  start = window.performance.now()

  // begin test
  // ...
  // end test

  duration = window.performance.now() - start

  log(duration)
}

test()
