#!/usr/bin/env node
var argv = require('yargs')
			.default('verbose', 0)
			.default('outdir', '../firmware')
			.default('hole1', '0x48,56')
			.default('hole2', '0xC8,52')
			.default('fstab', "filesystem.json")
			.default('version', Date.now())
			.argv;
var fs = require('fs');
var zlib = require('zlib');
var mime = require('mime');
var sprintf = require('sprintf-js').sprintf;

var fsTab = {};
var fsConfig = {};
var holes = [argv.hole1, argv.hole2].map(function (h) {
		var n = h.split(',').map(function (n) {
			return parseInt(n);
		});
		return { start: n[0], len: n[1], end: n[0]+n[1]-1 };
	});
//buildFs();
var fsChunks = readFS();
var fsTab = allocFS(fsChunks);
fsTab.fs_version = argv.version;
fs.writeFileSync(argv.fstab, JSON.stringify(fsTab));

function findFree() {
	for (var i = 0; i < holes.length; i++) {
		for (var j = 0; j < holes[i][1]; j++) {
			if (!fsTab[holes[i][0]+j])
				return holes[i][0]+j;
		}
	}
	throw("Out of mem");
}
function appendZeros(fname, d){
	if (d.length == 4*1024)
		return;
	var pad = new Buffer(4*1024 - d.length);
	fs.appendFileSync(fname, pad);
}
function allocFS(files) {
	// Allocate to maximize gap on top to allow rom binaries to grow without
	// impinging on the FS
	var nsec = 0;
	files.forEach(function (f) {
		nsec += f.sections.length;
	});
	//console.log("nsections:"+nsec);
	if (holes[0].len + holes[1].len < nsec) {
		throw(new Error("Can't allocate! Have "+(holes[0].len + holes[1].len)+" sections. Need "+nsec));
	}
	holes[0].use = Math.ceil((nsec + Math.abs(holes[0].len - holes[1].len))/2);
	holes[1].use = nsec - holes[0].use;
	if (holes[1].len > holes[0].len) {
		holes[1].use = holes[0].use;
		holes[0].use = nsec - holes[1].use;
	}
	holes.forEach(function (h) {
		h.alloc = h.start + h.len - h.use;
		h.cur = h.alloc;
		h.rem = h.use;
		h.data = new Buffer(0);
		h.flen = h.use*4*1024;
		h.fname = sprintf("/0x%X000.bin", h.alloc)
	});
	//console.log("holes", holes);
	files.sort(function (a,b) {
		return b.sections.length - a.sections.length;
	});
	var fsTab = { 
		blobs: holes.map(function (h) {
			return [h.alloc*4096, h.fname];
		})
	};
	files.forEach(function (f) {
		if (holes[0].rem >= f.sections.length) {
			holes[0].data = Buffer.concat([holes[0].data].concat(f.sections));
			fsTab[f.key] = f.headers.concat([[holes[0].cur, f.sections.length]]);
			holes[0].rem -= f.sections.length;
			holes[0].cur += f.sections.length;
		} else if (holes[1].rem >= f.sections.length) {
			holes[1].data = Buffer.concat([holes[1].data].concat(f.sections));
			fsTab[f.key] = f.headers.concat([[holes[1].cur, f.sections.length]]);
			holes[1].rem -= f.sections.length;
			holes[1].cur += f.sections.length;
		} else {
			holes[0].data = Buffer.concat([holes[0].data]
					.concat(f.sections.slice(0, holes[0].rem)));
			holes[1].data = Buffer.concat([holes[1].data]
					.concat(f.sections.slice(holes[0].rem)));
			fsTab[f.key] = f.headers.concat([
				[holes[0].cur, holes[0].rem],
				[holes[1].cur, f.sections.length - holes[0].rem]
			]);
			holes[0].cur += holes[0].rem;
			holes[1].cur +=  f.sections.length - holes[0].rem;
			holes[1].rem -= f.sections.length - holes[0].rem;
			holes[0].rem = 0;
		}
	});
	//console.log("filled holes", holes);
	holes.forEach(function (h) {
		var fname = argv.outdir + h.fname;
		console.log("File:"+fname+" Length:"+h.data.length);
		fs.writeFileSync(fname, h.data);
	});
	return fsTab;
}
function readFS() {
	var files = [];
	var tsize = 0;
	var nsec = 0;
	for (var i = 0; i < argv._.length; i++) {
		var d = argv._[i];
		//console.log("Reading dir", d);
		var f = fs.readdirSync(d);
		f.forEach(function (f) {
			var fname = d + '/' + f;
			//console.log("file=", fname, mime.lookup(f));
			var fd = fs.readFileSync(fname);
			var c = zlib.gzipSync(fd, {level:9});
			//console.log("c=", c.length, fd.length, c.length/fd.length*100, (fd.length-c.length)/1024);
			var headers = [
				["Content-type", mime.lookup(f)]
			];
			if (fd.length > 4*1024 && c.length < fd.length) {
				headers.push(["Content-Encoding", "gzip"]);
			} else
				c = fd;
			headers.push(["Content-Length", ""+c.length]);
			tsize += c.length;
			var sections = [];
			for (var i = 0; i < c.length; i += 4*1024) {
				var len = Math.min(4*1024, c.length - i);
				var sec = c.slice(i, i+len);
				if (len < 4*1024)
					sec = Buffer.concat([sec, new Buffer(4*1024-len)]);
				sections.push(sec);
				++nsec;
			}
			files.push({key: "file://"+f, headers: headers, sections: sections });
		});
	}
	console.log(sprintf("Total filesystem size=%.1f KB occupies=%dKB (%d/%d sections) "+
		"[Usage:%.1f%%, Wastage:%.1f%%]", 
		tsize/1024, nsec*4, nsec, holes[0].len+holes[1].len,
		nsec*100/(holes[0].len+holes[1].len), (nsec*4*1024-tsize)*100/tsize));
	return files;
}
process.exit(0);
/*
 * Test server -- to check gzip compression and http headers -- no longer used
var http = require('http');
http.createServer(function(request, response) {
	console.log(request.url);
	if (request.url == '/')
		request.url = "/index.html";
	var f = "file:/" + request.url;
	if (!fsConfig[f]) {
		response.statusCode = 404;
		response.statusMessage = 'Not found';
		response.setHeader("Content-Type", "text/plain");
		response.setHeader("Content-Length", 9);
		response.write("Not found");
		response.end();
		return;
	}
	response.statusCode = 200;
	for (var i = 0; i < fsConfig[f].length; i++) {
		if (isNaN(fsConfig[f][i][0]))
			response.setHeader(fsConfig[f][i][0], fsConfig[f][i][1]);
		else {
			for (var j = 0; j < fsConfig[f][i][1]; j++)
				response.write(fsTab[fsConfig[f][i][0] + j]);
		}
	}
	response.end();

}).listen(1337);
*/
