var mqtt    = require('mqtt');
var client  = mqtt.connect('mqtt://localhost');
 
client.on('connect', function () {
  console.log("connect");
  client.subscribe('zmote/widget/#', {qos: 2});
});
 
client.on('message', function (topic, message) {
  // message is Buffer 
  console.log("message", topic, 	message.toString());
  //client.end();
});
