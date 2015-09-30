/*
 *  Test basic function call.
 */

if (typeof window == 'undefined') window = {}
if (!window.performance) window.performance = {}
window.performance.now = window.performance.now || Date.now

if (typeof print != 'undefined') log = print
else log = console.log

function f () { return }

function test () {
  var start
  var duration

  start = window.performance.now()

  // begin test
  for (var i = 0; i < 1e8; i++) f()
  // end test

  duration = window.performance.now() - start

  log(duration)
}

test()
