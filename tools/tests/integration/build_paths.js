"use strict";

const path = require("path");

const rootDir = path.resolve(__dirname, "../../..");
const platformDir = { win32: "webrtc_vst_win", darwin: "webrtc_vst_mac", linux: "webrtc_vst_linux" };
if (!platformDir[process.platform]) {
    throw new Error("Unsupported test platform: " + process.platform);
}
const buildDir = path.resolve(rootDir, process.env.WEBRTC_TEST_BUILD_DIR || path.join("build", platformDir[process.platform]));
const configuration = process.env.WEBRTC_TEST_BUILD_CONFIG || "Release";
const cliExecutable = path.join(buildDir, "bin", configuration,
    process.platform === "win32" ? "webrtc_vst_cli_host.exe" : "webrtc_vst_cli_host");
const pluginBundle = path.join(buildDir, "VST3", configuration, "webrtc_vst.vst3");

module.exports = { rootDir, cliExecutable, pluginBundle };
