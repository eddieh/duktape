// get the window.location.search
var query = window.location.search

// get just the json file
var dataQuery = query.split('=')[1]

// allow the query string to have comma separated result files
// e.g. data=results-123.js,results-492.js,results-341.js

var dataPaths = dataQuery.split(',')

dataPaths.forEach(function (dataPath) {
  // construct a script tag to get the json file content
  var script = document.createElement('script')
  script.src = dataPath
  script.onload = combineData
  document.body.appendChild(script)
})

var processed = 0
var combinedData = []

function combineData () {
  data.forEach(function (test) {
    var record = combinedData.find(function (r) {
      return r.name == test.name
    })
    if (record) {
      // combine
      Object.keys(test.runs).forEach(function (key) {
        // don't overwrite runs if version numbers match
        var newKey = key
        var dupCount = 0
        while (record.runs[newKey] !== undefined) {
          dupCount++
          newKey = key + ' (' + dupCount + ')'
        }
        record.runs[newKey] = test.runs[key]
      })
    } else {
      combinedData.push(test)
    }
  })
  if (++processed == dataPaths.length) process()
}

function process () {
  combinedData.forEach(function (test) {
    var article = document.querySelectorAll('article')[0]
    var section = document.createElement('section')
    var heading = document.createElement('h2')
    var div = document.createElement('div')

    section.id = test.name
    heading.textContent = test.name
    div.className = 'chart'

    section.appendChild(heading)
    section.appendChild(div)
    article.appendChild(section)

    var keys = Object.keys(test.runs).sort()
    var values = []
    keys.forEach(function (key) {
      values.push(test.runs[key])
    })

    var x = d3.scale.linear()
          .domain([0, d3.max(values)])
          .range([0, 640])

    var record = d3.select(div)
          .selectAll('div').data(values)
          .enter().append('div').classed('record', true)

    record
      .append('div').classed('label', true)
      .text(function (d, i) { return keys[i] })

    record
      .append('div').classed('bar', true)
      .style('width', function (d) { return x(d) + 'px' })
      .html('&nbsp;')

    record
      .append('div').classed('value', true)
      .text(function (d) { return d })
  })
}
