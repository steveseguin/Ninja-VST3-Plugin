const WebSocket = require("ws");

const stream = process.argv[2] || "cli_loopback_job808d64";
const url = "wss://wss.vdo.ninja";

const ws = new WebSocket(url);

ws.on("open", () => {
    console.log("play open");
    ws.send(JSON.stringify({ request: "play", streamID: stream }));
    console.log("play sent");
});

ws.on("message", (msg) => {
    console.log("play message", msg.toString());
});

ws.on("close", () => {
    console.log("play close");
    process.exit(0);
});

ws.on("error", (err) => {
    console.error("play error", err);
});

setTimeout(() => {
    console.log("play timeout");
    ws.close();
}, 5000);
