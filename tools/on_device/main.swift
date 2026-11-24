// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// compass-on-device: the browser's way to Apple Intelligence on the Mac
// (docs/design/ai-sidebar.md, "On this device"). The browser starts it for
// a request and speaks OnDeviceLines over its stdin and stdout; the model
// code is CompassKit's CompassOnDevice, shared with iOS.

await OnDeviceLines.serve()
