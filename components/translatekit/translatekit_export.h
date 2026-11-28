// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_TRANSLATEKIT_TRANSLATEKIT_EXPORT_H_
#define COMPASS_COMPONENTS_TRANSLATEKIT_TRANSLATEKIT_EXPORT_H_

// The library's exports, found by name with dlsym or GetProcAddress.
#if defined(_WIN32)
#define TRANSLATEKIT_EXPORT __declspec(dllexport)
#else
#define TRANSLATEKIT_EXPORT __attribute__((visibility("default")))
#endif

#endif  // COMPASS_COMPONENTS_TRANSLATEKIT_TRANSLATEKIT_EXPORT_H_
