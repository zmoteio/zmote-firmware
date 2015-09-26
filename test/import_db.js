var argv = require('yargs')
    //.default('db', 'mongodb://localhost/zmote-server-dev')
    .default('url', 'http://zmote.herokuapp.com/')
    .default('herokuApp', 'zmote')
    .default('jdb', './remotes.json')
    .argv; // flash, bless, ssid, listvar Q = require('q');

var Q = require('q');
var mongoose = require('mongoose');
var _ = require('lodash');
var fs = require('fs');
var cproc = require('child_process');
var sleep = require('deasync')(function(timeout, done) {
    setTimeout(done, timeout);
});

require('./zmote-dbmodels/remote.server.model.js');
var Remote = mongoose.model('Remote');

var remotes = require(argv.jdb);
console.log(remotes.length + ' remotes to import');
var n = 0;
var rec;
function saveNext() {
	if (!rec) {
		if (remotes.length == 0) {
			console.log("Finished");
			process.exit(0);
		}
		var remote = remotes.shift();
		rec = new Remote(remote);
		console.log("Saving:", ++n);
	} else
		console.log("Re-save", rec._id);
	rec.save()
		.then(function () {
			console.log("Saved");
			rec = null;
			return saveNext();
		}, reStart);

}
function reStart(err) {
	if (err)
		console.error(err.stack);
	//	process.exit(2);
	connectDB()
		.then(saveNext, reStart);
}

reStart();
function connectDB() {
    var deferred = Q.defer();
 	var db = argv.db;
    if (!db) {
        cproc.execSync('heroku config:get -a '+ argv.herokuApp + ' MONGOLAB_URI')
            .toString()
            .split('\r\n')
            .forEach(function (l) {
                if (l.match(/^mongodb:\/\//))
                    db = l;
            });
            if (!db)
                throw(new Error("Can't find mongoDB URI.  Login to heroku first?"+l));
    }
    if (mongoose.connection)
        mongoose.connection.close();
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
