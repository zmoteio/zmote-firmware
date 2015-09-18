#!/bin/env node

var argv = require('yargs')

.usage('Usage: $0 <command> [options]')
    .command('bless', 'Create bless firmware and (optionally) write it to connected zmote[*]')
    .command('list', 'List zmotes in the network')
    .command('update', 'Trigger f/w update for zmote in the network')
    .command('updatefs', 'Push new file system to widget')
    .command('check', 'Check if connected zmote as been correctly blessed[*]')
    .command('ota_cycle', 'OTA cycle test (see --nupdate and --nupdatefs)')
    .command('fs_update_cycle', 'FS update test (see --nupdatefs)')
    .command('\t[*]', 'By "connected" we mean a USB-to-UART connection via CH340g')
    .demand(1)
    .example('$0 bless --keep-fw', 'Create binary to bless zmote')
    /*.demand('f')
    .alias('f', 'file')
    .nargs('f', 1)
    .describe('f', 'Load a file')*/
    .help('h')
    .alias('h', 'help')
    .option('mqtt', {
        describe: 'host:port of the mqttserver',
        nargs: 1,
        default: '104.154.71.241:2883'
    })
    .default('fstab', "../firmware/zmote-firmware_fs.json")
    .default('fw', "../firmware")
    .default('baud', 921600)
    .default('vid', "VID_1A86")
    .default('toolchain', '../../zmote-toolchain')
    .default('listPorts', '..\\..\\zmote-toolchain\\bin\\listComPorts.exe')
    .default('espTool', 'c:\\python27\\python.exe ..\\..\\zmote-toolchain\\bin\\esptool.py')
    .default('auth', './.client_auth')
    .default('url', 'http://zmote.herokuapp.com/')
    .default('herokuApp', 'zmote')
    //.default('bin', 'zmote')
    .default('cfgAddr', '7f')
    .default('keep-fw', false)
    .default('port', 8140)
    .default('rom0', '/rom0.bin')
    .default('rom1', '/rom1.bin')
    .option('nupdate', {
        describe: 'Number of times to run OTA update cycle',
        nargs: 1,
        default: '10'
    })
    .option('nupdatefs', {
        describe: 'Number of times to run FS update (for each OTA update in case of command="ota_cycle"',
        nargs: 1,
        default: '3'
    })
    .argv; // bless, update, list, help

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
var crc32 = require('crc-32').buf;

require('./zmote-dbmodels/widget.server.model.js');
var Widget = mongoose.model('Widget');
var state = {};

if (argv._.length == 0)
    argv._.push('all');
else if (argv._.length > 1) {
    console.error("Only one op may be specified at a time")
} else {
    runOp(argv._[0])
        .then(function() {
            process.exit(0);
        }, function(err) {
            console.error(err);
            console.error(err.stack);
            process.exit(2);
        });
}

function fixDB() {
    var db = argv.db;
    if (!db) {
        console.log("Getting DB credentials...");
        cproc.execSync('heroku config:get -a ' + argv.herokuApp + ' MONGOLAB_URI')
            .toString()
            .split('\r\n')
            .forEach(function(l) {
                if (l.match(/^mongodb:\/\//))
                    db = l;
            });
        if (!db)
            throw (new Error("Can't find mongoDB URI.  Login to heroku first?" + l));
        argv.db = db;
    }
    return db;
}

function connectDB() {
    var deferred = Q.defer();
    var db = fixDB();
    if (mongoose.connection)
        mongoose.connection.close();
    console.log("Connecting to DB...");
    mongoose.connect(db, function(err) {
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

function getFlashFiles() {
    var ret = new Buffer({
        length: 1024 * 1024
    });

    fs.readdirSync(argv.fw)
        .forEach(function(fn) {
            if (!fn.match(/^0x[0-9a-f]+[.]bin$/i))
                return;
            var addr = parseInt(fn.replace(/[.]bin$/, ""));
            var buf = fs.readFileSync(argv.fw + '/' + fn);
            console.log("Copying " + fn + " to " + addr);
            buf.copy(ret, addr);
        });
    return ret;
}

function getMac() {
    if (argv['chipid']) {
        state.chipID = argv['chipid'];
        console.log("Chip ID: " + state.chipID);
        return Q.when(true);
    }
    var com;
    (cproc.execSync(argv.listPorts + ' -vid ' + argv.vid).toString() || '')
    .split('\r\n')
        .forEach(function(ln) {
            if (!ln.length)
                return;
            if (com)
                throw (new Error("More than one connected ch430 found"));
            state.port = ln.replace(/\s.*$/, "");
        });
    if (!state.port)
        throw (new Error("No connected ch430 found"));
    console.log("Found com port: ", state.port);

    var cmd = argv.espTool + ' --port ' + state.port + ' --baud ' + argv.baud +
        ' read_mac';
    (cproc.execSync(cmd).toString() || '')
    .split('\r\n')
        .forEach(function(ln) {
            if (ln.match(/^MAC:/)) {
                state.mac = ln.replace(/^MAC:\s*/, '');
            }
        });
    if (!state.mac)
        throw (new Error("Didn't get mac address"));

    state.chipID = '00' + state.mac.split(':').splice(3, 3).join('');
    console.log("Got mac: " + state.mac);
    console.log("Chip ID: " + state.chipID);
    return Q.when(true);

}

function zmoteBless() {
    var fname = argv.bin;
    if (!fname)
        fname = "firmware_" + state.chipID + ".bin";
    var fsTab = JSON.parse(fs.readFileSync(argv.fstab));
    var cmd;
    return connectDB()
        .then(function() {
            return Widget.find({
                chipID: state.chipID
            });
        })
        .then(function(w) {
            if (!w || w.length == 0)
                return new Widget({
                    chipID: state.chipID
                });
            else if (w.length == 1)
                return w[0];
            else
                throw (new Error("More than one widget with chip ID found.  Fix DB first"));
        })
        .then(function(widget) {
            if (!argv['keep-fw']) cproc.execSync([argv.espTool, '--port', state.port, 'erase_flash'].join(' '));
            var flash = getFlashFiles();

            var config = {
                serial: widget._id,
                secret: secret(),
                mqtt_server: argv.mqtt.split(':')[0],
                mqtt_port: argv.mqtt.split(':')[1],
                mqtt_keepalive: 90
            };
            for (var f in fsTab) {
                if (!fsTab.hasOwnProperty(f))
                    continue;
                config[f] = fsTab[f];
            }
            var jStr = JSON.stringify(config);
            //console.log("jStr", jStr.length, jStr);
            var cfgAddr = 0x80000;
            flash.write(jStr, cfgAddr);
            flash.write(jStr, cfgAddr + 4096);
            // Zero-fill free space
            for (var i = jStr.length; i < 4096 - 12; i++) {
                flash.writeInt8(0, cfgAddr + i);
                flash.writeInt8(0, cfgAddr + 4096 + i);
            }
            flash.writeUInt32LE(0, cfgAddr + i); // flags
            flash.writeUInt32LE(1, cfgAddr + i + 4); // seq
            flash.writeInt32LE(
                crc32(flash.slice(cfgAddr, cfgAddr + 4096 - 4)),
                cfgAddr + i + 8); // crc
            flash.writeUInt32LE(0, cfgAddr + i + 4096); // flags
            flash.writeUInt32LE(2, cfgAddr + i + 4 + 4096); // seq
            flash.writeInt32LE(
                crc32(flash.slice(cfgAddr + 4096, cfgAddr + 2 * 4096 - 4)),
                cfgAddr + i + 8 + 4096); // crc
            fs.writeFileSync(fname, flash);
            fs.writeFileSync("config.bin", flash.slice(cfgAddr, cfgAddr + 2 * 4096));
            cmd = argv.espTool + ' --port ' + state.port + ' --baud ' + argv.baud +
                ' write_flash  -fs 8m 0x0 ' + fname;
            if (!argv['keep-fw']) {
                console.log("Flashing...");
                cproc.execSync(cmd);
                fs.unlinkSync(fname);
            }
            widget.secret = config.secret;
            return widget.save();
        })
        .then(function() {
            if (!argv['keep-fw'])
                return true;
            console.log("Created pre-blessed image in " + fname);
            console.log("Please run: ");
            console.log("\t../../zmote-toolchain/bin/esptool.py --port " + state.port + ' --baud ' + argv.baud +
                ' write_flash  -fs 8m 0x0 ' + fname);
            console.log("\t../../zmote-toolchain/bin/esptool.py --port " + state.port + " run");
        });
}

function zmoteCheck() {
    var fname = secret() + '.bin';
    return connectDB()
        .then(function() {
            return Widget.find({
                chipID: state.chipID
            });
        })
        .then(function(w) {
            if (!w || w.length != 1)
                throw (new Error("DB error"));
            var widget = w[0];
            var cmd = argv.espTool + ' --port ' + state.port + ' --baud ' + argv.baud +
                ' read_flash ' + '0x' + argv.cfgAddr + '000 12288 ' + fname;
            cproc.execSync(cmd);
            var rawConfig = fs.readFileSync(fname);
            var jStr = readJsonStr(rawConfig);
            fs.unlinkSync(fname);
            //console.log(jStr);
            var config = JSON.parse(jStr);
            if (config.chipID != state.chipID || config.serial != widget._id || config.secret != widget.secret)
                throw (new Error("ID/secret mismatch"));
            if (widget.mac != state.mac || config.mac != state.mac)
                throw (new Error("MAC mismatch"));
            console.log("zMote OK");
        });
}

function zmoteReboot() {
    if (argv['keep-fw']) return;
    var cmd = argv.espTool + ' --port ' + state.port + ' run';
    cproc.execSync(cmd);
    sleep(1000);
}

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
var prnWidgets = true;
function getWidgets(max) {
    state.widget = null;
    return clientAuth()
        .then(function(auth) {
            return axios.post(argv.url + 'widgets', {
                client: auth
            });
        })
        .then(function(resp) {
            var w = resp.data.filter(function(w) {
                return w.connected;
            });
            if (!w || w.length == 0)
                throw (new Error("No widgets found"));
            if (prnWidgets)
                w.forEach(function(w, i) {
                    console.log("[" + i + "] Found widget: " + w.chipID);
                    console.log("    LocalIP: " + w.localIP);
                    console.log("    ExtIP: " + w.extIP);
                    if (argv.chipid && w.chipID == argv.chipid)
                        state.widget = w;
                });
            prnWidgets = false;
            if (argv.chipid) {
                if (state.widget)
                    return true;
                else
                    throw(new Error("Widget "+argv.chipid+" not found"));
            } else if (max && w.length > max)
                throw (new Error("Too many widgets: " + JSON.stringify(w)));
            state.widget = w[0];
            return true;
        });
}

function mqttConnect() {
    var mqtt = require('mqtt');
    var q = Q.defer();
    console.log("Connecting to MQTT broker...");
    var client = mqtt.connect({
        host: argv.mqtt.split(':')[0],
        port: argv.mqtt.split(':')[1],
        clientId: 'admin_factory',
        username: argv.db.replace(/^mongodb:\/\//, '').replace(/:.*/, ''),
        password: argv.db.replace(/^mongodb:\/\/.*?:/, '').replace(/@.*/, '')
    });
    client.on('connect', function() {
        console.log("Connected to MQTT broker");
        q.resolve(client);
    });
    client.on('error', function(err) {
        console.log("MQTT error");
        q.reject(err);
    });
    return q.promise;
}

function localIP() {
    var ip;
    cproc.execSync('netsh interface ip show config name="Wi-Fi"')
        .toString()
        .split('\r\n')
        .forEach(function(ln) {
            if (!ln.match(/^\s*IP Address\s*:/))
                return;
            ip = ln.replace(/^\s*IP Address\s*:\s*/, "").replace(/\s*$/, "");
            console.log("Local IP: " + ip);
        });
    if (!ip)
        throw (new Error("Local IP not found"));
    return ip;
}

function zmoteUpdateFS() {
    var fserveDone;
    var msg = JSON.parse(fs.readFileSync(argv.fstab));
    var nfiles = msg.blobs.length;
    return connectDB()
        .then(function() {
            fserveDone = serveHttp(nfiles);
            return mqttConnect();
        })
        .then(function(client) {
            var q = Q.defer();
            var done = false;
            var w = state.widget;
            msg.command = "updatefs";
            msg.ip = localIP();
            msg.port = argv.port;
            console.log("Publish update message to zmote/towidget/" + w.chipID, msg);
            client.publish("zmote/towidget/" + w.chipID, JSON.stringify(msg), {
                qos: 1
            }, function() {
                done = true;
                q.resolve(true);
            });
            setTimeout(function() {
                if (!done)
                    q.reject("Timeout");
            }, 60000);
        })
        .then(function() {
            console.log("Message sent");
            setTimeout(function () {
                fserveDone.reject(new Error("timeout"));
            }, nfiles*60*1000);
            return fserveDone.promise;
        });
}

function serveHttp(nfiles) {
    var static = require('node-static');
    var fserveDone = Q.defer();
    var fileServer = new static.Server('../firmware');
    var filesServed = 0;
    console.log("Starting HTTP server...");
    var server = require('http').createServer(function(request, response) {
        request.addListener('end', function() {
            fileServer.serve(request, response, function(err, result) {
                if (err) { // There was an error serving the file
                    console.error("Error serving " + request.url + " - " + err.message);

                    // Respond to the client
                    response.writeHead(err.status, err.headers);
                    response.end();
                    server.close();
                    fserveDone.reject(err);
                }
                ++filesServed;
                console.log("Finished serving file %d/%d", filesServed, nfiles);
                //process.exit(0);
                if (filesServed == nfiles)
                    setTimeout(function() {
                        server.close();
                        fserveDone.resolve(true);
                    }, 10000);
            });
        }).resume();
    });
    server.listen(argv.port);
    //argv.port++;
    return fserveDone;
}

function zmoteUpdate() {
    var fserveDone;
    return connectDB()
        .then(function() {
            fserveDone = serveHttp(1);
            return mqttConnect();
        })
        .then(function(client) {
            var q = Q.defer();
            var done = false;
            var msg = JSON.stringify({
                command: "update",
                ip: localIP(),
                port: argv.port,
                rom0: argv.rom0,
                rom1: argv.rom1
            });
            var w = state.widget;
            console.log("Publish update message to zmote/towidget/" + w.chipID, msg);
            client.publish("zmote/towidget/" + w.chipID, msg, {
                qos: 1
            }, function() {
                done = true;
                q.resolve(true);
            });
            setTimeout(function() {
                if (!done)
                    q.reject("Timeout");
            }, 60000);
        })
        .then(function() {
            console.log("Message sent");
            setTimeout(function() {
                fserveDone.reject(new Error("timeout"));
            }, 60 * 1000);
            return fserveDone.promise;
        });
}

function getStatus() {
    var url = 'http://'+state.widget.localIP+'/';//+state.widget.sta_mac+'/api/wifi/status';
    return axios.get(url+'api/wifi/mac')
        .then(function (resp) {
            url = url + resp.data.sta_mac + '/api/wifi/status';
            //console.log("status url:"+url);
            return axios.get(url);
        })
        .then(function (resp) {
            return resp.data;
        });
}
function updateFSCycle(nupdate) {
    var fsv;
    if (!nupdate)
        return Q.when(true);
    console.log("Starting updateFS:"+nupdate);
    cproc.execSync('make -C .. fs'); // Updates the time stamp (used as version)
                                     // to force zmote to accept

    return getWidgets(1)
            .then(getStatus)
            .then(function (s) {
                fsv = s.fs_version;
                cproc.execSync('node factory.js updatefs');
            })
            //.then(zmoteUpdateFS)
            .then(getStatus)
            .then(function (s) {
                if (s.fs_version == fsv) {
                    console.log("UpdateFS fail.  Retry");
                    //throw(new Error("Update FS failed"));
                } else {
                    console.log("updateFS success: "+nupdate);
                    --nupdate;
                }
                return updateFSCycle(nupdate);
            });
    return 
}
function updateCycle(nupdate) {
    var rom;
    if (!nupdate)
        return Q.when(true);
    console.log("Starting update cycle:"+nupdate);
    return getWidgets(1)
            .then(getStatus)
            .then(function (s) {
                rom = s.boot;
                cproc.execSync('node factory.js update');
            })
            //.then(zmoteUpdate)
            .then(function () {
                return Q.delay(20000);
            })
            .then(function () {
                return getWidgets(1);
            })
            .then(getStatus)
            .then(function (s) {
                if (s.boot !== 1 - rom) {
                    console.log("Update failed.  Retry");
                    return updateCycle(nupdate);
                    //throw(new Error("Update failed:"+nupdate));
                }
                console.log("update success: "+nupdate);
            })
            .then(function () {
                return updateFSCycle(argv.nupdatefs);
            })
            .then(function () {
                return updateCycle(nupdate - 1);
            });
}
function runOp(op) {
    switch (op) {
        case 'bless':
            return getMac()
                .then(zmoteBless)
                .then(zmoteReboot);
        case 'list':
            return getWidgets();
        case 'update':
            return getWidgets(1)
                .then(zmoteUpdate);
        case 'updatefs':
            return getWidgets(1)
                .then(zmoteUpdateFS);
        case 'check':
            return getMac()
                .then(zmoteCheck);
        case 'fs_update_cycle':
            return updateFSCycle(argv.nupdatefs);
        case 'ota_cycle':
            return updateCycle(argv.nupdate);
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
