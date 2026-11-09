#!/bin/sh
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# zypak runs Chromium's sandbox through Flatpak's, without setuid.
export CHROME_WRAPPER=/app/bin/compass
export CHROME_DESKTOP=io.github.Compass_Browser.Compass.desktop
exec zypak-wrapper /app/compass/chrome --no-default-browser-check "$@"
