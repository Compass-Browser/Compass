// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The prefs the window's rebuilt panes show, through SettingsWindowHandler's
// allowlist (GetPrefs(), SetPref(), PrefChanged()) rather than
// settingsPrivate. Values are JSON over Mojo.

import type {SettingsPref} from '../sidebar.mojom-webui.js';

import {SettingsWindowBrowserProxy} from './browser_proxy.js';

export interface PrefValue {
  value: unknown;
  // Set by policy or an extension: shown, not changeable.
  enforced: boolean;
}

type Listener = (name: string, pref: PrefValue) => void;

class PrefsStore {
  private values_ = new Map<string, PrefValue>();
  private listeners_ = new Set<Listener>();
  private listening_ = false;

  get(name: string): PrefValue|undefined {
    return this.values_.get(name);
  }

  // Loads |names| (the allowlisted ones) and follows their changes.
  async load(names: string[]): Promise<void> {
    const proxy = SettingsWindowBrowserProxy.getInstance();
    if (!this.listening_) {
      this.listening_ = true;
      proxy.callbackRouter.prefChanged.addListener(
          (pref: SettingsPref) => this.update_(pref));
    }
    const {prefs} = await proxy.handler.getPrefs(names);
    for (const pref of prefs) {
      this.update_(pref);
    }
  }

  // Sets |name|; false if the browser refused it.
  async set(name: string, value: unknown): Promise<boolean> {
    const {ok} = await SettingsWindowBrowserProxy.getInstance().handler.setPref(
        name, JSON.stringify(value));
    return ok;
  }

  addListener(listener: Listener) {
    this.listeners_.add(listener);
  }

  removeListener(listener: Listener) {
    this.listeners_.delete(listener);
  }

  private update_(pref: SettingsPref) {
    const value = {value: JSON.parse(pref.valueJson), enforced: pref.enforced};
    this.values_.set(pref.name, value);
    for (const listener of this.listeners_) {
      listener(pref.name, value);
    }
  }
}

export const prefs = new PrefsStore();
