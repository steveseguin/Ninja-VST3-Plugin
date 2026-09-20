"use strict";
const assert = require("node:assert/strict");
const path = require("node:path");
const { execFileSync } = require("node:child_process");
const { test } = require("node:test");
const root = path.resolve(__dirname, "../../..");
function paths(overrides) {
    const env = { ...process.env };
    for (const key of ["WEBRTC_TEST_BUILD_DIR", "WEBRTC_TEST_BUILD_CONFIG", "WEBRTC_TEST_PLUGIN_BUNDLE"]) delete env[key];
    return JSON.parse(execFileSync(process.execPath, ["-e",
        "process.stdout.write(JSON.stringify(require('./tools/tests/integration/build_paths')))"],
    { cwd: root, env: { ...env, ...overrides }, encoding: "utf8" }));
}
test("default bundle follows the chosen build/configuration", () => {
    const result = paths({ WEBRTC_TEST_BUILD_DIR: "build/path-test", WEBRTC_TEST_BUILD_CONFIG: "Debug" });
    assert.equal(result.pluginBundle, path.join(root, "build/path-test/VST3/Debug/webrtc_vst.vst3"));
});
test("installed bundle override changes the plugin without changing the CLI host", () => {
    const defaults = paths({});
    for (const candidate of ["build/download smoke/webrtc_vst.vst3", path.join(root, "build/installed/webrtc_vst.vst3")]) {
        const result = paths({ WEBRTC_TEST_PLUGIN_BUNDLE: candidate });
        assert.equal(result.pluginBundle, path.resolve(root, candidate));
        assert.equal(result.cliExecutable, defaults.cliExecutable);
    }
});
