// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSettingsAboutPaneElement} from './about_pane.js';
import {PREF_UPDATE_CHANNEL} from './about_pane.js';

export function getHtml(this: CompassSettingsAboutPaneElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<header class="about-header">
  <img class="about-logo" alt="" src="/settings_window/compass_logo.svg">
  <h2 class="about-name">Compass</h2>
  <span class="about-version" id="version">${this.getVersionText_()}</span>
  <span class="about-chromium">${this.getChromiumText_()}</span>
</header>

<section class="row-group">
  <h2 class="group-title">Updates</h2>
  <div class="group">
    <div class="row control-row" data-row="updates">
      <span class="tile" data-color="${this.getUpdateColor_()}">
        <compass-settings-icon name="update"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${this.getUpdateLabel_()}</span>
        <span class="row-detail" id="updateStatus">
          ${this.getUpdateText_()}
        </span>
      </span>
      <button class="pill" ?hidden="${!this.canCheck_()}"
          @click="${this.onCheckClick_}">
        Check for Updates
      </button>
      <button class="pill accent" ?hidden="${!this.isUpdateReady_()}"
          @click="${this.onInstallClick_}">
        Relaunch to Update
      </button>
    </div>
    <div class="row control-row" data-row="channel">
      <span class="tile" data-color="indigo">
        <compass-settings-icon name="pulse"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Update channel</span>
        <span class="row-detail">
          Beta and Canary get new features first, with more bugs.
        </span>
      </span>
      <select class="popup" aria-label="Update channel"
          ?disabled="${this.isEnforced_(PREF_UPDATE_CHANNEL)}"
          @change="${this.onChannelChange_}">
        ${this.updateChannels.map(channel => html`
          <option value="${channel.value}" ?selected="${
              channel.value === this.getString_(PREF_UPDATE_CHANNEL)}">
            ${channel.label}
          </option>`)}
      </select>
    </div>
  </div>
</section>

<section class="row-group">
  <div class="group">
    <button class="row" data-row="licenses" @click="${this.onLicensesClick_}">
      <span class="tile" data-color="gray">
        <compass-settings-icon name="doc"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Open-source licenses</span>
      </span>
      <compass-settings-icon class="row-trail" name="external"
          title="Opens in a tab">
      </compass-settings-icon>
    </button>
  </div>
</section>
<!--_html_template_end_-->`;
  // clang-format on
}
