// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// No "Restore pages? Compass didn't shut down correctly." bubble after a
// crash. Compass keeps the sidebar (pinned entries, Today) itself and brings
// it back on launch, its tabs unloaded until clicked (ARCHITECTURE.md, D14),
// so after a crash the tabs are there already; the bubble only offered
// upstream's session restore of them. For the same reason the "Continue where
// you left off" infobar is off (browser/feature_overrides.cc).
//
// Both of upstream's callers (startup's infobar_utils.cc and
// SessionService) go through this one function.

#define ShowSessionCrashedBubbleIfNotOffTheRecordProfile \
  ShowSessionCrashedBubbleIfNotOffTheRecordProfile_ChromiumImpl
#include "src/chrome/browser/ui/views/session_crashed_bubble_view.cc"
#undef ShowSessionCrashedBubbleIfNotOffTheRecordProfile

void ShowSessionCrashedBubbleIfNotOffTheRecordProfile(
    BrowserWindowInterface* browser,
    bool skip_tab_checking);

void ShowSessionCrashedBubbleIfNotOffTheRecordProfile(
    BrowserWindowInterface* browser,
    bool skip_tab_checking) {}
