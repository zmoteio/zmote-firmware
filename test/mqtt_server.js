    var config = require('./mqtt-config');
var mosca = require('mosca');
var mongoose = require('mongoose');
var Q = require('q');
require('./zmote-dbmodels/widget.server.model.js');
var Widget = mongoose.model('Widget');
var connectDB = (function () {
    var deferred = Q.defer();
    mongoose.connect(config.db, function(err) {
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

})();

var settings = {
    host: '0.0.0.0',
    port: config.port,
    backend: config.backend,
    persistence: config.persistence,
    http: config.http,
    observer: {
        host: 'localhost',
        port: config.port,
        clientId: secret(),
        username: secret(),
        password: secret(),
    }
};


console.log("settings", settings);
var server = new mosca.Server(settings, function (err) {
    if (err)
        console.error("Error starting mosca", err);
    console.log("Server started");
});

server.on('clientConnected', function(client) {
    console.log('client connected', client.id);
});
server.on('clientDisconnected', function(client) {
    console.log('client disconnected', client.id);
});

server.on('error', function(err) {
    console.error('server error', err);
});
server.on('ready', setup);

var authenticate = function (client, username, password, callback) {
    var auth = false;
    password = password.toString();
    if (username == settings.observer.username && password == settings.observer.password) {
        callback(null, true);
        return;
    }
    connectDB
        .then(function () {
            return Widget.findById(username);
        })
        .then(function (widget) {
            if (!widget) {
                console.log("Widget "+username+" not found");
                return true;
            }
            if (client.id == widget.chipID && widget._id == username && widget.secret == password) {
                console.log(client.id + " auth suceeded");
                client.type = 'widget';
                console.log("IP: ", client.connection.stream.remoteAddress);
                auth = true;
                widget.extIP = client.connection.stream.remoteAddress;
                return widget.save();
            } else {
                console.log(client.id + " auth FAIL");
                console.log(client.id, username, password, widget);
                return true;
            }
        })
        .then(function () {
            console.log("Auth finished");
            callback(null, auth);
        }, function (err) {
            console.error("MDB search error", err.stack);
            callback(null, auth);
        });
};

var authorizePublish = function(client, topic, payload, callback) {
    var path = topic.split('/');
    //console.log("Auth Publish");
    //console.log("client.type", client.type);
    //console.log("topic", topic, payload.toString());
    if (client.id == settings.observer.clientId) {
        callback(null, true);
        return;
    }
    if (path.length !== 3 || path[0] !== 'zmote' || path[1]  !== 'widget' || path[2] !== client.id) {
        console.log("publish disallowed: client=\""+client.id+"\" topic=\""+topic+"\"", path);
        callback("auth denied", false);
    } else {
        //console.error("Publish allowed");
        callback(null, true);
    }
};

var authorizeSubscribe = function(client, topic, callback) {
    var path = topic.split('/');
    //console.log("Auth Subscribe");
    //console.log("client.type", client.type);
    //console.log("topic", topic);
    if (client.id == settings.observer.clientId) {
        callback(null, true);
        return;
    }
    //console.log("auth sub", client.id, topic);
    if (path.length !== 3 || path[0] !== 'zmote' || path[1] !== 'towidget' || path[2] !== client.id) {
        console.log("subscribe disallowed: client=\""+client.id+"\" topic=\""+topic+"\"");
        callback("auth denied", false);
    } else
        callback(null, true);
};

function setup() {
    console.log('zMote broker is up and running');
    server.authenticate = authenticate;
    server.authorizePublish = authorizePublish;
    server.authorizeSubscribe = authorizeSubscribe;
    //server.published = function (packet, client, callback) {
    //    console.log("published", packet.topic);
    //    callback(null);
    //};
    var mqtt    = require('mqtt');
    var client  = mqtt.connect(settings.observer);
     
    client.on('connect', function () {
      client.subscribe('zmote/widget/#');
    });
     
    client.on('message', function (topic, message) {
      // message is Buffer 
      var msg = JSON.parse(message.toString());
      console.log("Message", msg);
      var path = topic.split('/');
      connectDB
        .then(function () {
            return Widget.findOne({chipID: path[2]});
        })
        .then(function (widget) {
            if (msg.disconnected) {
                widget.connected = false;
            } else {
                widget.localIP = msg.ip;
                widget.connected = true;
            }
            widget.lastEvent = new Date();
            console.log("widget", widget, widget.save);
            return widget.save();
        })
        .then(function () {
            console.log("Widget record updated");
        }, function (err) {
            console.error("Unexpected error", err.stack);
        });
    });
}

function secret() {
    var allChars = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890';
    var base = '';
    for (var i = 0; i < 64; i++)
        base += allChars.charAt(Math.floor(Math.random() * allChars.length));
    return base;
}