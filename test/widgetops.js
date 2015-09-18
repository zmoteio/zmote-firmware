#!/bin/env node

var argv = require('yargs')
    .default('db', 'mongodb://localhost/zmote-server-dev')
    .default('url', 'http://localhost:3000/')
    .default('mqtt', '104.154.71.241:1883')
    .default('fstab', "../firmware/zmote-firmware_fs.json")
    .default('fw', "../firmware")
    .default('baud', 460800)
    .default('vid', "VID_1A86")
    .default('toolchain', '../../zmote-toolchain')
    .default('wifi', 'klar:ksplUsers120')
    .default('listPorts', '..\\..\\zmote-toolchain\\bin\\listComPorts.exe')
    .default('espTool', 'c:\\python27\\python.exe ..\\..\\zmote-toolchain\\bin\\esptool.py')
    .default('auth', './.client_auth')
    .argv; // flash, bless, ssid, list, add
// all=flash, bless, ssid, check

var axios = require('axios');
var Q = require('q');
var sprintf = require("sprintf-js").sprintf;
var mongoose = require('mongoose');
var _ = require('lodash');
var fs = require('fs');
var cproc = require('child_process');
var sleep = require('deasync')(function(timeout, done) {
    setTimeout(done, timeout);
});

require('./zmote-dbmodels/widget.server.model.js');
var Widget = mongoose.model('Widget');

var state = {
    connected: false
};

if (argv._.length == 0)
    argv._.push('all');
else if (argv._.length > 1) {
    console.error("Only one op may be specified at a time")
} else
    runOp(argv._[0])
    .then(function() {
        process.exit(0);
    }, function(err) {
        console.error(err);
        console.error(err.stack);
        process.exit(2);
    });

function clientAuth() {
    var auth;
    if (fs.existsSync(argv.auth)) {
        auth = JSON.parse(fs.readFileSync(argv.auth));
        return Q.when(auth);
    }
    return axios.get(argv.url + 'client/register')
        .then(function(resp) {
            fs.writeFileSync(argv.auth, JSON.stringify(resp.data));
            return resp.data;
        });

}

function saveId() {
    var auth;
    return clientAuth()
        .then(function(a) {
            auth = a;
            return axios.post(argv.url + 'widgets', auth);
        })
        .then(function(resp) {
            if (resp.data.length == 1)
                state.listedDevice = resp.data[0];
            else
                console.log("Not saving Id", resp.data);
        });
}

function saveConnection() {
    cproc.execSync('netsh wlan show interfaces')
        .toString()
        .split('\r\n')
        .forEach(function(ln) {
            if (!ln.match(/^\s*Profile\s*:/))
                return;
            if (state.savedConnection)
                throw (new Error("More than one wifi profile found"));
            state.savedConnection = ln.replace(/^\s*Profile\s*:\s*/, "").replace(/\s*$/, "");
            console.log("Currently connected to: " + state.savedConnection);
        });
    if (state.savedConnection)
        return Q.when(state.savedConnection);
    return Q.reject("Not found");
}

function checkConnection(profile) {
    cproc.execSync('netsh wlan show interfaces')
        .toString()
        .split('\r\n')
        .forEach(function(ln) {
            if (!ln.match(/^\s*Profile\s*:/))
                return;
            var n = ln.replace(/^\s*Profile\s*:\s*/, "").replace(/\s*$/, "");
            if (n != profile)
                throw (new Error("Unable to connect to " + profile + " (connected to:" + n + ")"));
            console.log("Connected to "+profile);
        });
    return Q.when(true);
}

function reConnect() {
    if (!state.savedConnection)
        throw (new Error("No saved connection"));
    console.log("Connecting to " + state.savedConnection);
    cproc.execSync('netsh wlan connect name=' + state.savedConnection);
    state.connected = false;
    sleep(3000);
    return checkConnection(state.savedConnection);
}

function connectDB() {
    var deferred = Q.defer();
    if (mongoose.connection)
        mongoose.connection.close();
    mongoose.connect(argv.db, function(err) {
        if (err) {
            console.error('Could not connect to MongoDB!');
            console.log(err);
            deferred.reject(err);
        } else {
            console.log("Connected to DB");
            deferred.resolve(true);
        }
    });
    return deferred.promise;

}

function prepareDB() {
    return connectDB()
        .then(function() {
            if (state.listedDevice) {
                return Widget.find({
                    chipID: state.listedDevice.chipID
                });
            } else if (state.widget) {
                return state.widget;
            } else {
                var w = new Widget();
                return w;
            }
        })
        //.then(function(widget) {
        //    return widget.save();
        //})
        .then(function (widget) {
            console.log("Prepared widget: "+widget._id);
            state.widget = widget;
        });
}

function zmoteConnect() {
    var connected = false,
        n = 0;
    while (!connected && n < 3) {
        cproc.execSync('netsh wlan show networks')
            .toString()
            .split('\r\n')
            .forEach(function(ln) {
                if (ln.match("ZMOTE_NEW")) {
                    cproc.execSync('netsh wlan connect name=ZMOTE_NEW');
                    connected = true;
                }
            });
        if (connected)
            break;
        cproc.execSync('netsh wlan refresh');
        sleep(3000);
        ++n;
        console.log("ZMOTE_NEW not found.  Retry " + n);
    }
    if (!connected) // Give it a shot anyway
        cproc.execSync('netsh wlan connect name=ZMOTE_NEW');
    sleep(10000);
    state.connected = true;
    return checkConnection("ZMOTE_NEW");
}

function zmoteFlash() {
    var com;
    (cproc.execSync(argv.listPorts + ' -vid ' + argv.vid).toString() || '')
    .split('\r\n')
        .forEach(function(ln) {
            if (!ln.length)
                return;
            if (com)
                throw (new Error("More than one connected ch430 found"));
            com = ln.replace(/\s.*$/, "");
        });
    if (!com)
        throw (new Error("No connected ch430 found"));
    console.log("Found com port: ", com);
    var f = [];
    fs.readdirSync(argv.fw)
        .forEach(function(fn) {
            if (!fn.match(/^0x[0-9a-f]+[.]bin$/i))
                return;
            f.push(fn.replace(/[.]bin$/, ""));
            f.push(argv.fw + '/' + fn);
        });
    console.log("Flashing: " + f.join(' '));
    var cmd = argv.espTool + ' --port ' + com + ' --baud ' + argv.baud +
        ' write_flash -fs 8m ' + f.join(' ');
    console.log("Command:\n\t"+cmd);
    cproc.execSync(cmd);
    console.log("Done");
    return Q.when(true);
}

function zmoteJoin() {
    console.log("getting mac");
    var url = 'http://zmote.io/';
    return axios.get(url + 'api/wifi/mac')
        .then(function(resp) {
            if (!resp.data || !resp.data.sta_mac) {
                throw ("Bad format");
            }
            url = url + resp.data.sta_mac + '/api/';
            console.log("joining", resp.data);
            return axios.put(url + 'wifi/connect', {
                ssid: argv.wifi.split(':')[0],
                password: argv.wifi.split(':')[1]
            });
        })
        .then(function(resp) {
            console.log("Connect OK?", resp.data);
            return true;
        });
}

function zmoteUrl() {
    var url;
    if (state.connected)
        url = 'http://zmote.io/';
    else
        url = 'http://' + state.widget.localIP + '/';
    console.log("zmote url: "+url);
    return axios.get(url + 'api/wifi/mac')
        .then(function(resp) {
            if (!resp.data || !resp.data.sta_mac) {
                throw ("Bad format");
            }
            state.url = url + resp.data.sta_mac + '/api/';
            return state.url;
        });
}
function zmoteBless() {
    console.log("Bless");
    var widget, url, id;
    var fsTab = JSON.parse(fs.readFileSync(argv.fstab));
    if (state.widget)
        widget = Q.when(state.widget);
    else
        widget = prepareDB();
    return widget
        .then(function(widget) {
            id = widget._id;
            return zmoteUrl();
        })
        .then(function(u) {
            url = u;
            return axios.get(url + 'spi/00');
        })
        .then(function(resp) {
            config = resp.data;
            //console.log("config", config);
            config.serial = id;
            config.secret = secret();
            config.mqtt_server = argv.mqtt.split(':')[0];
            config.mqtt_port = argv.mqtt.split(':')[1];
            config.mqtt_keepalive = 120;
            for (var f in fsTab) {
                if (!fsTab.hasOwnProperty(f))
                    continue;
                config[f] = fsTab[f];
            }
            // Change the AP SSID to zMote_<mac_id[24:0]>
            //    return axios.put(url + 'wifi/config', {
            //    	ssid: "zMote_" + config.mac.split(':').slice(3, 6).join('')
            //    });
            //})
            //.then(function (resp) {
            return axios.put(url + 'spi/00', config);
        })
        .then(function(resp) {
            console.log("Write OK?", resp.data);
            return axios.get(url + 'spi/00');
        })
        .then(function(resp) {
            //console.log(config, resp.data);
            //if (!_.isEqual(resp.data, config)) // FIXME need a way to check
            //	throw("Mismatch");
            console.log("Finished writing to SPI flash");
            state.config = resp.data;
            return resp.data;
        });
}

function saveRec() {
    if (!state.config)
        throw (new Error("Unable to find config"));
    var widget = Q.when(state.widget);
    return Widget.find({
            chipID: state.config.chipID
        })
        .then(function(w) {
            var p = [];
            if (!state.widget)
                state.widget = w.pop();
            for (var i = 0; i < w.length; i++)
                if (w[i]._id != state.widget._id)
                    p.push(w[i].remove());
            return Q.all(p);
        })
        .then(function () {
            if (!state.widget)
                throw(new Error("Nothing to save"));
            console.log("Saving widget: "+state.widget._id);
            for (var f in state.config) {
                if (!state.config.hasOwnProperty(f))
                    continue;
                //console.log(f+"="+config[f]);
                state.widget[f] = config[f];
            }
            console.log("Saving DB entry");
            return state.widget.save();
        });
}

function zmoteGetIP() {
    return connectDB()
        .then(function () {
            if (state.widget)
                return Widget.find({_id: state.widget._id});
            else
                return Widget.find({connected: true});
        })
        .then(function (widgets) {
            console.log("widgets", widgets);
            if (widgets.length == 0)
                throw(new Error("No widgets found"));
            state.widgets = widgets;
            widgets.forEach(function (w) {
                console.log((w.connected?"Connected: ":"Not Connected")
                    +w._id+
                    (w.connected?"("+w.localIP+")":""));
            });
            return widgets;
        });
}
function zmoteUniqSSID() {
    
    return zmoteUrl()
        .then(function (url) {
            axios.put(state.url + 'wifi/config', {
                ssid: "zMote_" + state.config.mac.split(':').slice(3, 6).join('')
            });
        })
}
function addZmotes() {
    if (argv._.length < 2)
        throw(new Error("add must specify chipID"));
    var p = [];
    for (var i = 1; i < argv._.length; i++) {
        p.push(
            Widget.find({
                chipID: argv._[i]
            })
            .then(function(w) {
                var p = [];
                if (!state.widget)
                    state.widget = w.pop();
                for (var i = 0; i < w.length; i++)
                    if (w[i]._id != state.widget._id)
                        p.push(w[i].remove());
                return Q.all(p);
            })
            .then(function () {
                if (!state.widget)
                    throw(new Error("Nothing to save"));
                console.log("Saving widget: "+state.widget._id);
                for (var f in state.config) {
                    if (!state.config.hasOwnProperty(f))
                        continue;
                    //console.log(f+"="+config[f]);
                    state.widget[f] = config[f];
                }
                console.log("Saving DB entry");
                return state.widget.save();
            })
        );
    }
}
function runOp(op) {
    switch (op) {
        case 'flash':
            return zmoteFlash();
        case 'bless':
            return saveConnection()
                .then(prepareDB)
                .then(zmoteConnect)
                .then(zmoteBless)
                .then(reConnect)
                .then(prepareDB)
                .then(saveRec);
        case 'join':
            return saveConnection()
                .then(zmoteConnect)
                .then(zmoteJoin)
                .then(reConnect);
        case 'ssid':
            return zmoteGetIP()
                .then(zmoteUniqSSID);
        case 'list':
            return zmoteGetIP();
        case 'add':
            return prepareDB()
                .then(addZmotes)
                ;
        case 'all':
            return saveConnection()
                .then(prepareDB)
                .then(zmoteFlash)
                .then(zmoteConnect)
                .then(zmoteBless)
                .then(zmoteJoin)
                .then(reConnect)
                .then(prepareDB)
                .then(saveRec)
                .then(zmoteGetIP)
                .then(zmoteUniqSSID);
        case 'help':
        default:
            return help();
    }

}

function secret() {
    var allChars = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890';
    var base = '';
    for (var i = 0; i < 64; i++)
        base += allChars.charAt(Math.floor(Math.random() * allChars.length));
    return base;
}

/*
var fsTab = JSON.parse(fs.readFileSync(argv.fstab));

mongoose.connect(argv.db, function(err) {
	if (err) {
		console.error('Could not connect to MongoDB!');
		console.log(err);
	} else {
		console.log("Connected to DB")
		var rec = new Widget();
		blessZmote(fsTab, rec._id)
			.then(function (config) {
				//console.log("ret   ", config);
				for (var f in config) {
                    if (!config.hasOwnProperty(f))
                    	continue;
					//console.log(f+"="+config[f]);
                	rec[f] = config[f];
            	}
            	console.log("Saving DB entry");
				return rec.save();
			})
			.then(function () {
				console.log("Finished");
				process.exit(0);
			}, function (err) {
				console.error("Error:", err, err.stack);
				process.exit(2);
			});
	}
});

function blessZmote(fsTab, id) {
	var config;
	console.log("blessing ", id);
	return axios.get(url+'api/wifi/mac')
		.then(function (resp) {
			//console.log(">>>>>>>>>>>>>>response", resp.data);
			if (!resp.data || !resp.data.sta_mac) {
				throw("Bad format");
			}
			url = url + resp.data.sta_mac + '/api/';
			return axios.get(url+'spi/00');
		})
		.then(function (resp) {
			config = resp.data;
			//console.log("config", config);
            config.serial = id;
            config.secret = secret();
            config.mqtt_server = argv.mqtt.split(':')[0];
            config.mqtt_port = argv.mqtt.split(':')[1];
            config.mqtt_keepalive = 120;
			for (var f in fsTab) {
                if (!fsTab.hasOwnProperty(f))
                    continue;
                config[f] = fsTab[f];
            }
 			// Change the AP SSID to zMote_<mac_id[24:0]>
        //    return axios.put(url + 'wifi/config', {
        //    	ssid: "zMote_" + config.mac.split(':').slice(3, 6).join('')
        //    });
		//})
		//.then(function (resp) {
			return axios.put(url+'spi/00', config);
		})
		.then(function (resp) {
			console.log("Write OK?", resp.data);
			return axios.get(url+'spi/00');
		})
		.then(function (resp) {
			//console.log(config, resp.data);
			//if (!_.isEqual(resp.data, config)) // FIXME need a way to check
			//	throw("Mismatch");
			return resp.data;
		});
}

// From: https://gist.github.com/Maksims/6084210
function wifiNetworksRSSI(fn) {
  // prepare result string of data
  var res = '';
  var deferred = Q.defer();
  // spawn netsh with required settings
  var netsh = cproc.spawn('netsh', ['wlan', 'show', 'networks', 'mode=bssid']);

  // get data and append to main result
  netsh.stdout.on('data', function (data) {
    res += data;
  });

  // if error occurs
  netsh.stderr.on('data', function (data) {
    console.log('stderr: ' + data);
  });

  // when done
  netsh.on('close', function (code) {
    if (code == 0) { // normal exit
      // prepare array for formatted data
      var networks = [ ];
      // split response to blocks based on double new line
      var raw = res.split('\r\n\r\n');

      // iterate through each block
      for(var i = 0; i < raw.length; ++i) {
        // prepare object for data
        var network = { };

        // parse SSID
        var match = raw[i].match(/^SSID [0-9]+ : (.+)/);
        if (match && match.length == 2) {
          network.ssid = match[1];
        } else {
          network.ssid = '';
        }

        // if SSID parsed
        if (network.ssid) {
          // parse BSSID
          var match = raw[i].match(' +BSSID [0-9]+ +: (.+)');
          if (match && match.length == 2) {
            network.bssid = match[1];
          } else {
            network.bssid = '';
          }

          // parse RSSI (Signal Strength)
          var match = raw[i].match(' +Signal +: ([0-9]+)%');
          if (match && match.length == 2) {
            network.rssi = parseInt(match[1]);
          } else {
            network.rssi = NaN;
          }

          // push to list of networks
          networks.push(network);
        }
      }

      // callback with networks and raw data
      //fn(null, networks, res);
      deferred.resolve(networks);
    } else {
      // if exit was not normal, then throw error
      //fn(new Error(code));
      deferred.reject(new Error(code));
    }
  });
  return deferred.promise;
}
*/
