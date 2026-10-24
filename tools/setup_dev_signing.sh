#!/bin/sh
# Copyright 2026 The Strata Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Creates a self-signed code signing identity, "Strata Dev Signing", in the
# login keychain. `strata build` and `strata test` sign strata_browsertests
# with it after each build, so macOS keeps the "removable volume" permission
# across relinks instead of asking again (it remembers an ad-hoc signed
# binary by its hash, which changes on every link). Run once, on macOS.

set -eu

NAME="Strata Dev Signing"
if security find-certificate -c "$NAME" >/dev/null 2>&1; then
  echo "\"$NAME\" is already in the keychain."
  exit 0
fi

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cat > "$dir/cert.cnf" <<EOF
[req]
distinguished_name = dn
prompt = no
x509_extensions = ext
[dn]
CN = $NAME
[ext]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
EOF
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout "$dir/key.pem" -out "$dir/cert.pem" -config "$dir/cert.cnf"
pass=$(openssl rand -hex 12)
openssl pkcs12 -export -inkey "$dir/key.pem" -in "$dir/cert.pem" \
  -name "$NAME" -out "$dir/id.p12" -passout "pass:$pass"
security import "$dir/id.p12" -k "$HOME/Library/Keychains/login.keychain-db" \
  -P "$pass" -T /usr/bin/codesign
echo "Added \"$NAME\". The next \`strata test --browser\` signs with it;"
echo "click Allow once more, and macOS should remember it from then on."
