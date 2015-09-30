/*
 *  Fibonacci test, exercises recursion.
 */

if (typeof window == 'undefined') window = {}
if (!window.performance) window.performance = {}
window.performance.now = window.performance.now || Date.now

if (typeof print != 'undefined') log = print
else log = console.log

function fibonacci (n) {
  return n <= 1 ? n : fibonacci(n - 2) + fibonacci(n - 1)
}

function test () {
  var start
  var duration

  start = window.performance.now()

  // begin test
  fibonacci(35)
  // end test

  duration = window.performance.now() - start

  log(duration)
}

test()
