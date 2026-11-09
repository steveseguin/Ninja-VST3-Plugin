"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { rootDir, pluginBundle } = require("../integration/build_paths");

test("release versions agree across source, package and built VST3 metadata", t => {
    const header = fs.readFileSync(path.join(rootDir, "webrtc_vst/src/Version.h"), "utf8");
    const version = header.match(/#define WEBRTC_VST_VERSION_STRING "([^"]+)"/)[1];
    const components = ["MAJOR", "MINOR", "PATCH"].map(name =>
        header.match(new RegExp("#define WEBRTC_VST_VERSION_" + name + " (\\d+)"))[1]);
    assert.equal(components.join("."), version);
    assert.equal(require(path.join(rootDir, "package.json")).version, version);
    const metadata = path.join(pluginBundle, "Contents/Resources/moduleinfo.json");
    if (!fs.existsSync(metadata)) return t.skip("Build the VST3 bundle to validate release metadata");
    const info = JSON.parse(fs.readFileSync(metadata, "utf8").replace(/,(\s*[}\]])/g, "$1"));
    assert.equal(info.Version, version, "Bundle version differs from the plugin source");
    for (const entry of info.Classes) assert.equal(entry.Version, version, "Exported class version differs");
});
