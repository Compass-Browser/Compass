// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSettingsAdvancedPaneElement} from './advanced_pane.js';
import {PREF_CARET_BROWSING, PREF_ENERGY_SAVER, PREF_FOCUS_HIGHLIGHT, PREF_LIVE_CAPTION, PREF_MEMORY_SAVER} from './advanced_pane.js';

export function getHtml(this: CompassSettingsAdvancedPaneElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<section class="row-group" data-row="performance">
  <h2 class="group-title">Performance</h2>
  <div class="group">
    <label class="row control-row" data-row="memorySaver">
      <span class="tile" data-color="green">
        <compass-settings-icon name="leaf"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Memory saver</span>
        <span class="row-detail">
          Frees memory from tabs you haven't used in a while.
        </span>
      </span>
      <input type="checkbox" class="switch"
          .checked="${this.isMemorySaverOn_()}"
          ?disabled="${this.isEnforced_(PREF_MEMORY_SAVER)}"
          @change="${this.onMemorySaverChange_}">
    </label>
    <div class="row control-row" data-row="energySaver">
      <span class="tile" data-color="yellow">
        <compass-settings-icon name="bolt"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Energy saver</span>
        <span class="row-detail">
          Limits background activity and visual effects.
        </span>
      </span>
      <select class="popup" aria-label="Energy saver"
          ?disabled="${this.isEnforced_(PREF_ENERGY_SAVER)}"
          @change="${this.onEnergySaverChange_}">
        ${this.energySaverModes.map(mode => html`
          <option value="${mode.value}" ?selected="${
              mode.value === this.getNumber_(PREF_ENERGY_SAVER)}">
            ${mode.label}
          </option>`)}
      </select>
    </div>
  </div>
</section>

<section class="row-group" data-row="accessibility">
  <h2 class="group-title">Accessibility</h2>
  <div class="group">
    <label class="row control-row" data-row="liveCaption">
      <span class="tile" data-color="blue">
        <compass-settings-icon name="caption"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Live caption</span>
        <span class="row-detail">Captions for audio and video in English.</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_LIVE_CAPTION}"
          .checked="${this.getBool_(PREF_LIVE_CAPTION)}"
          ?disabled="${this.isEnforced_(PREF_LIVE_CAPTION)}"
          @change="${this.onSwitchChange_}">
    </label>
    <label class="row control-row" data-row="focusHighlight">
      <span class="tile" data-color="purple">
        <compass-settings-icon name="focus"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Highlight the focused object</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_FOCUS_HIGHLIGHT}"
          .checked="${this.getBool_(PREF_FOCUS_HIGHLIGHT)}"
          ?disabled="${this.isEnforced_(PREF_FOCUS_HIGHLIGHT)}"
          @change="${this.onSwitchChange_}">
    </label>
    <label class="row control-row" data-row="caretBrowsing">
      <span class="tile" data-color="slate">
        <compass-settings-icon name="cursor"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Navigate pages with a text cursor</span>
        <span class="row-detail">Caret browsing; F7 turns it on or off.</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_CARET_BROWSING}"
          .checked="${this.getBool_(PREF_CARET_BROWSING)}"
          ?disabled="${this.isEnforced_(PREF_CARET_BROWSING)}"
          @change="${this.onSwitchChange_}">
    </label>
  </div>
</section>

<section class="row-group" data-row="reset">
  <h2 class="group-title">Reset</h2>
  <div class="group">
    <div class="row control-row">
      <span class="tile" data-color="red">
        <compass-settings-icon name="reset"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Reset settings</span>
        <span class="row-detail">${this.getResetText_()}</span>
      </span>
      <button class="pill" ?hidden="${this.confirmingReset_}"
          ?disabled="${this.resetting_}" @click="${this.onResetClick_}">
        Reset…
      </button>
    </div>
    <div class="row control-row confirm-row" ?hidden="${!this.confirmingReset_}">
      <span class="row-text">
        <span class="row-detail">
          Resets the search engine, site permissions and other browser
          settings, clears cookies and turns off extensions. Your spaces,
          tabs, history and passwords stay.
        </span>
      </span>
      <button class="pill" @click="${this.onResetCancelClick_}">Cancel</button>
      <button class="pill danger" @click="${this.onResetConfirmClick_}">
        Reset
      </button>
    </div>
  </div>
</section>
<!--_html_template_end_-->`;
  // clang-format on
}
