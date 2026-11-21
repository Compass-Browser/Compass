// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSettingsPasswordsPaneElement} from './passwords_pane.js';
import {PREF_AUTO_SIGN_IN, PREF_OFFER_TO_SAVE, PREF_SAVE_ADDRESSES, PREF_SAVE_CARDS} from './passwords_pane.js';

export function getHtml(this: CompassSettingsPasswordsPaneElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<section class="row-group">
  <h2 class="group-title">Passwords</h2>
  <div class="group">
    <label class="row control-row" data-row="offerToSave">
      <span class="row-text">
        <span class="row-label">Offer to save passwords and passkeys</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_OFFER_TO_SAVE}"
          .checked="${this.getBool_(PREF_OFFER_TO_SAVE)}"
          ?disabled="${this.isEnforced_(PREF_OFFER_TO_SAVE)}"
          @change="${this.onSwitchChange_}">
    </label>
    <label class="row control-row" data-row="autoSignIn">
      <span class="row-text">
        <span class="row-label">Sign in automatically</span>
        <span class="row-detail">
          On sites where you saved one password.
        </span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_AUTO_SIGN_IN}"
          .checked="${this.getBool_(PREF_AUTO_SIGN_IN)}"
          ?disabled="${this.isEnforced_(PREF_AUTO_SIGN_IN)}"
          @change="${this.onSwitchChange_}">
    </label>
    <button class="row" data-row="passwordManager"
        @click="${this.onPasswordManagerClick_}">
      <span class="row-text">
        <span class="row-label">Password Manager</span>
        <span class="row-detail">
          Add and edit passwords, checkup, import and export.
        </span>
      </span>
      <compass-settings-icon class="row-trail" name="external"
          title="Opens in a tab">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group" data-row="savedPasswords">
  <div class="group-header">
    <h2 class="group-title">Saved passwords · ${this.getPasswordsDetail_()}</h2>
    <input class="field filter" type="search" placeholder="Filter"
        aria-label="Filter saved passwords" spellcheck="false"
        .value="${this.filter_}" @input="${this.onFilterInput_}">
  </div>
  <div class="group">
    ${this.getShownPasswords_().map(password => html`
      <div class="row control-row password-row">
        <span class="row-text">
          <span class="row-label">${password.site}</span>
          <span class="row-detail">${password.username}</span>
        </span>
        <span class="secret" ?revealed="${this.isRevealed_(password)}">
          ${this.getSecret_(password)}
        </span>
        <button class="pill small" data-id="${password.id}"
            ?hidden="${password.isPasskey}" @click="${this.onRevealClick_}">
          ${this.getRevealLabel_(password)}
        </button>
        <button class="pill small" data-id="${password.id}"
            ?hidden="${password.isPasskey}" @click="${this.onCopyClick_}">
          ${this.getCopyLabel_(password)}
        </button>
        <button class="pill small" data-id="${password.id}"
            aria-label="Remove ${password.site}"
            @click="${this.onRemovePasswordClick_}">
          Remove
        </button>
      </div>`)}
    <p class="empty" ?hidden="${this.getShownPasswords_().length > 0}">
      No saved passwords.
    </p>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">Payment methods</h2>
  <div class="group">
    <label class="row control-row" data-row="saveCards">
      <span class="row-text">
        <span class="row-label">Save and fill payment methods</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_SAVE_CARDS}"
          .checked="${this.getBool_(PREF_SAVE_CARDS)}"
          ?disabled="${this.isEnforced_(PREF_SAVE_CARDS)}"
          @change="${this.onSwitchChange_}">
    </label>
    ${this.cards_.map(card => html`
      <div class="row control-row">
        <span class="row-text">
          <span class="row-label">${this.getCardText_(card)}</span>
          <span class="row-detail">Expires ${card.expiration}</span>
        </span>
        <button class="pill small" data-guid="${card.guid}"
            ?hidden="${!card.removable}" @click="${this.onRemoveCardClick_}">
          Remove
        </button>
      </div>`)}
    <button class="row" data-row="payments" data-sub-page="payments"
        @click="${this.onChromeSettingsClick_}">
      <span class="row-text">
        <span class="row-label">Add or edit cards</span>
      </span>
      <compass-settings-icon class="row-trail" name="external"
          title="Opens in a tab">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">Addresses</h2>
  <div class="group">
    <label class="row control-row" data-row="saveAddresses">
      <span class="row-text">
        <span class="row-label">Save and fill addresses</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_SAVE_ADDRESSES}"
          .checked="${this.getBool_(PREF_SAVE_ADDRESSES)}"
          ?disabled="${this.isEnforced_(PREF_SAVE_ADDRESSES)}"
          @change="${this.onSwitchChange_}">
    </label>
    ${this.addresses_.map(address => html`
      <div class="row control-row">
        <span class="row-text">
          <span class="row-label">${address.name}</span>
          <span class="row-detail">${address.summary}</span>
        </span>
        <button class="pill small" data-guid="${address.guid}"
            @click="${this.onRemoveAddressClick_}">
          Remove
        </button>
      </div>`)}
    <button class="row" data-row="addresses" data-sub-page="contactInfo"
        @click="${this.onChromeSettingsClick_}">
      <span class="row-text">
        <span class="row-label">Add or edit addresses</span>
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
