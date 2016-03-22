#!/bin/env node

var http = require('http');
var request = require('request');
var q = require('q');

var argv = require('yargs')
    .usage('Usage: $0 --localip=localip')
    .help('h')
    .alias('h', 'help')
    .demand(['localip'])
    .argv;

console.log(argv.localip)
ip = argv.localip;
var sta_mac = '18-fe-34-a5-ef-49';
var q1 = q.defer();

var get_stac_mac = function() {
    var qu = q.defer();
    request({
            url: 'http://' + ip + '/api/wifi/mac',
            json: true
        },
        function(error, resp, body) {
            if (!error && resp.statusCode == 200) {
                console.log(body.sta_mac);
                sta_mac = body.sta_mac;
                qu.resolve();
            } else
                console.log("error", error, resp, body);
            qu.reject();
        }
    );
    return qu.promise;
};

var test_trigger = function() {
    var qu = q.defer();
    request({
            url: 'http://' + ip + '/' + sta_mac + '/api/ir/trigger',
            json: true
        },
        function(error, resp, body) {
            if (!error && resp.statusCode == 200) {
                console.log(body);
                qu.resolve();
            } else
                console.log("error", error, resp, body);
            qu.reject();
        }
    );
    return qu.promise;
}

var test_read = function() {
    var qu = q.defer();
    request({
            url: 'http://' + ip + '/' + sta_mac + '/api/ir/read',
            json: true
        },
        function(error, resp, body) {
            if (!error && resp.statusCode == 200) {
                console.log(JSON.stringify(body));
                qu.resolve();
            } else
                console.log("error", error, resp, body);
            qu.reject();
        }
    );
    return qu.promise;
};

var test_write = function() {
    var qu = q.defer();
    console.log('test_write', 'http://' + ip + '/' + sta_mac + '/api/ir/write');
    var bodyString = JSON.stringify({
        period: 910222,
        n: 42,
        seq: [96, 32, 16, 32, 16, 16, 16, 16, 16, 32, 32, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 32, 16, 16, 32, 16, 16, 16, 3021],
        repeat: [0, 0, 42]
    });

    var headers = {
        'Content-Type': 'application/json',
        'Content-Length': bodyString.length
    };

    var options = {
        host: ip,
        path: '/' + sta_mac + '/api/ir/write',
        method: 'PUT',
        headers: headers
    };

    var callback = function(resp) {
        //console.log(resp);
        qu.resolve();
    };

    http.request(options, callback).write(bodyString);
    return qu.promise;
};

var test_delay = function(delay) {
    if (!delay) delay = 2000;
    var qu = q.defer();
    setTimeout(function() {
        qu.resolve();
    }, delay);
    return qu.promise;
}

get_stac_mac()
.then(test_trigger)
.then(test_read)
.then(function(){
	console.log('press a real remote button now');
	return false;
})
.then(test_delay)
.then(test_read)
.then(test_trigger)
.then(test_read)
.then(function(){
	console.log('press a real remote button now');
	return false;
})
.then(test_delay)
.then(test_read)
.then(function(){
	console.log('get ready with phone camera');
	return false;
})
.then(test_delay)
.then(test_write)
.then(test_delay)
.then(test_write)
.then(test_delay)
.then(test_write)
.then(test_delay)
.then(test_write)
.done();
