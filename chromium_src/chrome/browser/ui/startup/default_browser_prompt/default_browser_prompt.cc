// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// No "Compass isn't your default browser" prompt at startup: Compass doesn't
// nag about defaults. Its Settings window's General pane sets the default
// browser (browser/ui/webui/settings/settings_window_handler_general.cc).
//
// Startup (infobar_utils.cc) chains the "pin to taskbar" and PDF viewer
// infobars after this prompt through |done_callback|; it isn't run, so those
// don't follow either (the PDF one is off as a feature too,
// browser/feature_overrides.cc).

#define ShowDefaultBrowserPrompt ShowDefaultBrowserPrompt_ChromiumImpl
#include "src/chrome/browser/ui/startup/default_browser_prompt/default_browser_prompt.cc"
#undef ShowDefaultBrowserPrompt

void ShowDefaultBrowserPrompt(Profile* profile,
                              base::OnceCallback<void(bool)> done_callback);

void ShowDefaultBrowserPrompt(Profile* profile,
                              base::OnceCallback<void(bool)> done_callback) {}
