const WebSocket = require("ws");
const ws = new WebSocket("wss://wss.vdo.ninja");

ws.on("open", () => {
    console.log("open");
    ws.send(JSON.stringify({ request: "seed", streamID: "teststream" }));
    console.log("seed sent");
});

ws.on("message", (data) => {
    console.log("message", data.toString());
});

ws.on("close", () => {
    console.log("close");
    process.exit(0);
});

ws.on("error", (err) => {
    console.error("error", err);
});

setTimeout(() => {
    console.log("timeout");
    ws.close();
}, 5000);
