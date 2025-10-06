const path = require("path");
const VDONinjaSDK = require(path.resolve(__dirname, "../../../js_sdk/vdoninja-sdk.js"));

const sdk = new VDONinjaSDK({
    host: "wss://wss.vdo.ninja",
    debug: true
});

sdk._log = (...args) => {
    const line = args.map((value) => (typeof value === "object" ? JSON.stringify(value) : String(value))).join(" ");
    console.log("[LOG]", line);
};

sdk._logMessage = (direction, msg, transport) => {
    console.log([] , JSON.stringify(msg));
};

sdk.addEventListener("connected", () => {
    console.log("[EVENT] connected");
});

sdk.addEventListener("publishing", (info) => {
    console.log("[EVENT] publishing", JSON.stringify(info));
});

sdk.addEventListener("error", (err) => {
    console.error("[EVENT] error", JSON.stringify(err));
});

sdk.connect().then(async () => {
    console.log("[RESULT] connect resolved");
    try {
        const streamId = await sdk.announce({ streamID: "sdk_probe_cli" });
        console.log("[RESULT] announce resolved", streamId);
    } catch (error) {
        console.error("[RESULT] announce failed", error);
    }
    setTimeout(() => {
        console.log("[RESULT] closing");
        try {
            sdk.disconnect();
        } catch (error) {
            console.error("[RESULT] disconnect failed", error);
        }
        process.exit(0);
    }, 3000);
}).catch((error) => {
    console.error("[RESULT] connect failed", error);
    process.exit(1);
});
