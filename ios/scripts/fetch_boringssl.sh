#!/bin/bash
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Puts BoringSSL into ios/CompassKit/Sources/CCryptoBoringSSL (gitignored).
#
# We take swift-crypto's copy: BoringSSL with every symbol prefixed
# (CCryptoBoringSSL_*) and laid out for SwiftPM. swift-crypto doesn't export
# that target as a product, so we copy it. It has what CryptoKit lacks and
# the desktop uses: SPAKE2 (pairing), scrypt (Nigori) and deterministic
# Ed25519 (the sync token), so the desktop's test vectors match byte for byte.
set -euo pipefail

SWIFT_CRYPTO_TAG=5.0.3

here="$(cd "$(dirname "$0")" && pwd)"
dest="$here/../CompassKit/Sources/CCryptoBoringSSL"
stamp="$dest/.swift-crypto-tag"

if [[ -f "$stamp" && "$(cat "$stamp")" == "$SWIFT_CRYPTO_TAG" && "${1:-}" != "--force" ]]; then
  echo "BoringSSL from swift-crypto $SWIFT_CRYPTO_TAG already in place"
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
git -c advice.detachedHead=false clone -q --depth 1 --branch "$SWIFT_CRYPTO_TAG" \
  https://github.com/apple/swift-crypto.git "$tmp/swift-crypto"

rm -rf "$dest"
mkdir -p "$dest"
cp -R "$tmp/swift-crypto/Sources/CCryptoBoringSSL/." "$dest/"
# SwiftPM doesn't use the CMake file. The networking files need <netdb.h> and
# nothing of ours uses them (swift-crypto excludes them too).
rm -f "$dest/CMakeLists.txt" "$dest/crypto/bio/connect.cc" \
  "$dest/crypto/bio/socket_helper.cc" "$dest/crypto/bio/socket.cc"
cp "$tmp/swift-crypto/LICENSE.txt" "$dest/SWIFT_CRYPTO_LICENSE.txt"
echo "$SWIFT_CRYPTO_TAG" > "$stamp"
echo "BoringSSL from swift-crypto $SWIFT_CRYPTO_TAG ($(tail -1 "$dest/hash.txt"))"
