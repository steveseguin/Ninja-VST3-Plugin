#!/bin/bash
set -euo pipefail
[[ $(uname -s) == Darwin ]] || { echo "This script requires macOS." >&2; exit 1; }
profile=${1:-vdoninja-notary}
echo "Store Apple notarization credentials in local Keychain profile: $profile"
echo "Use an Apple app-specific password, not your normal Apple ID password."
# Let notarytool prompt securely. Never put a password into this script or git.
exec xcrun notarytool store-credentials "$profile"
