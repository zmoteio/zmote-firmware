module.exports = {
	/*backend: {
		type: 'redis',
		redis: require('redis'),
		db: 12,
		return_buffers: true, // to handle binary payloads
  		host: "localhost",
        port: 6379,
	},*/
    db: 'mongodb://localhost/zmote-server-dev',
	port: 2883,
};

