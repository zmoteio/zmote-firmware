#!/usr/bin/env node

var axios = require('axios');
var Q = require('q');
var sprintf = require("sprintf-js").sprintf;

var url = 'http://192.168.0.104/18-fe-34-9b-1c-20/api/ir/';


function objForEach(obj, f) {
	for (var k in obj) {
		if (!obj.hasOwnProperty(k))
			continue;
		f(k, obj[k]);
	}
}
function guessMFreq(d) {
	var err = {36000: 0, 38000: 0, 40000: 0, 56000:0, 37900: 0};
	d.forEach(function (t) {
		if (t > 10000)
			return; // Ignore gaps
		objForEach(err, function (f) {
			var e = t*1e-6/(1/f);
			e = e - Math.floor(e);
			err[f] += Math.pow(Math.min(e, 1 - e), 2);			
		});
	});
	var max_e = d.length, best_f;
	for (var f in err) {
		if (err[f] < max_e) {
			max_e = err[f];
			best_f = f;
		}
	}
	objForEach(err, function (f, v) {
		err[f] += Math.sqrt(v)/d.length;			
	});
	console.log(err);
	console.log("Mod freq="+best_f);
	return 37900;//best_f;
}
function waitForCode(q) {
	axios.get(url+'trigger')
		.then(function () {
			console.log("Press key...");
			return Q.delay(2000);
		})
		.then(function () {
			return axios.get(url+'read');
		})
		.then(function (resp) {
			//console.log("data", resp.data);
			if (resp.data.trigger.length > 30) {
				console.log("Got it");
				q.resolve(resp.data);
			} else {
				console.log("Retry");
				return waitForCode(q);
			}
		}, function (err) {
			q.reject(err);
		});
}
function getCode() {
	var q = Q.defer();
	waitForCode(q);
	return q.promise;
}

getCode()
	.then(function (code) {
		//console.log(code);
		if (code.trigger[0] < 3000) {
			console.error("Sorry.  Retry");
			throw("Bad capture");
		}
		var f = guessMFreq(code.trigger);
		var e = code.trigger.length;
		for (var e = 0; e < code.trigger.length; e += 2) {
			if (code.trigger[e] > 10000) 
				break;
		}
		var seq = code.trigger.slice(0, e);
		//console.log(seq);
		process.stdout.write(sprintf("curl -H 'Content-Type: application/json' -X POST -d '{\"period\":%d, \"len\": %d, \"seq\": [", 
			Math.floor(65535/f/2*1e6), seq.length));
		seq.forEach(function (t, i) {
			process.stdout.write(sprintf("%s%d", i?",":"", Math.round(t*1e-6/(1/f))))
		});
		process.stdout.write("]}' "+url+'write\n');	
	}, function (err) {
		console.error("Err:", err);
	}).catch(console.error);
