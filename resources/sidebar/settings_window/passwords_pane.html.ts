// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSettingsPasswordsPaneElement} from './passwords_pane.js';
import {PREF_AUTO_SIGN_IN, PREF_OFFER_TO_SAVE, PREF_SAVE_ADDRESSES, PREF_SAVE_CARDS} from './passwords_pane.js';

export function getHtml(this: CompassSettingsPasswordsPaneElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.isView_('main') ? html`
<section class="row-group">
  <h2 class="group-title">Passwords</h2>
  <div class="group">
    <label class="row control-row" data-row="offerToSave">
      <span class="tile" data-color="green">
        <compass-settings-icon name="passwords"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Offer to save passwords and passkeys</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_OFFER_TO_SAVE}"
          .checked="${this.getBool_(PREF_OFFER_TO_SAVE)}"
          ?disabled="${this.isEnforced_(PREF_OFFER_TO_SAVE)}"
          @change="${this.onSwitchChange_}">
    </label>
    <label class="row control-row" data-row="autoSignIn">
      <span class="tile" data-color="blue">
        <compass-settings-icon name="signin"></compass-settings-icon>
      </span>
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
    <button class="row" data-row="savedPasswords" data-view="passwords"
        @click="${this.onViewClick_}">
      <span class="tile" data-color="gray">
        <compass-settings-icon name="lock"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Saved passwords</span>
      </span>
      <span class="row-count">${this.passwords_.length}</span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
    <button class="row" data-row="passwordManager"
        @click="${this.onPasswordManagerClick_}">
      <span class="tile" data-color="indigo">
        <compass-settings-icon name="privacy"></compass-settings-icon>
      </span>
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

<section class="row-group">
  <h2 class="group-title">Payment methods</h2>
  <div class="group">
    <label class="row control-row" data-row="saveCards">
      <span class="tile" data-color="green">
        <compass-settings-icon name="card"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Save and fill payment methods</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_SAVE_CARDS}"
          .checked="${this.getBool_(PREF_SAVE_CARDS)}"
          ?disabled="${this.isEnforced_(PREF_SAVE_CARDS)}"
          @change="${this.onSwitchChange_}">
    </label>
    <button class="row" data-row="cards" data-view="cards"
        @click="${this.onViewClick_}">
      <span class="tile" data-color="indigo">
        <compass-settings-icon name="card"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Cards</span>
      </span>
      <span class="row-count">${this.cards_.length}</span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">Addresses</h2>
  <div class="group">
    <label class="row control-row" data-row="saveAddresses">
      <span class="tile" data-color="green">
        <compass-settings-icon name="home"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Save and fill addresses</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_SAVE_ADDRESSES}"
          .checked="${this.getBool_(PREF_SAVE_ADDRESSES)}"
          ?disabled="${this.isEnforced_(PREF_SAVE_ADDRESSES)}"
          @change="${this.onSwitchChange_}">
    </label>
    <button class="row" data-row="addresses" data-view="addresses"
        @click="${this.onViewClick_}">
      <span class="tile" data-color="orange">
        <compass-settings-icon name="home"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">Addresses</span>
      </span>
      <span class="row-count">${this.addresses_.length}</span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>` : nothing}
${this.isView_('passwords') ? html`
<button class="back" @click="${this.onBackClick_}">
  <compass-settings-icon name="back"></compass-settings-icon>
  Passwords
</button>
<h2 class="subpage-title">Saved passwords</h2>
<section class="row-group" data-row="savedPasswords">
  <div class="group-header">
    <h2 class="group-title">${this.getPasswordsTitle_()}</h2>
    <input class="field filter" type="search" placeholder="Filter"
        aria-label="Filter saved passwords" spellcheck="false"
        .value="${this.filter_}" @input="${this.onFilterInput_}">
  </div>
  <div class="group">
    ${this.getShownPasswords_().map(password => html`
      <div class="row control-row password-row">
        <span class="tile letter"
            data-color="${this.getSiteColor_(password.site)}">
          ${this.getSiteInitial_(password.site)}
        </span>
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
</section>` : nothing}
${this.isView_('cards') ? html`
<button class="back" @click="${this.onBackClick_}">
  <compass-settings-icon name="back"></compass-settings-icon>
  Passwords
</button>
<h2 class="subpage-title">Cards</h2>
<section class="row-group">
  <div class="group">
    ${this.cards_.map(card => html`
      <div class="row control-row">
        <span class="tile" data-color="indigo">
          <compass-settings-icon name="card"></compass-settings-icon>
        </span>
        <span class="row-text">
          <span class="row-label">${this.getCardText_(card)}</span>
          <span class="row-detail">Expires ${card.expiration}</span>
        </span>
        <button class="pill small" data-guid="${card.guid}"
            ?hidden="${!card.removable}" @click="${this.onEditCardClick_}">
          Edit
        </button>
        <button class="pill small" data-guid="${card.guid}"
            ?hidden="${!card.removable}" @click="${this.onRemoveCardClick_}">
          Remove
        </button>
      </div>`)}
    <p class="empty" ?hidden="${this.cards_.length > 0}">No saved cards.</p>
  </div>
  <div class="actions" ?hidden="${!!this.cardDraft_}">
    <button class="pill" data-row="addCard"
        @click="${this.onAddCardClick_}">
      Add Card…
    </button>
  </div>
  ${this.cardDraft_ ? html`
  <h2 class="group-title draft-title">${this.getCardDraftTitle_()}</h2>
  <div class="group">
    <div class="row control-row">
      <span class="row-text"><span class="row-label">Name on card</span></span>
      <input class="field" data-field="nameOnCard" aria-label="Name on card"
          autocomplete="off" .value="${this.cardDraft_.nameOnCard}"
          @input="${this.onCardInput_}">
    </div>
    <div class="row control-row" ?hidden="${!this.isNewCard_()}">
      <span class="row-text"><span class="row-label">Card number</span></span>
      <input class="field" data-field="number" aria-label="Card number"
          inputmode="numeric" autocomplete="off"
          .value="${this.cardDraft_.number}" @input="${this.onCardInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">Expires</span></span>
      <select class="popup" data-field="expirationMonth"
          aria-label="Expiry month" @change="${this.onCardExpiryChange_}">
        ${this.months.map(month => html`
          <option value="${month}"
              ?selected="${month === this.cardDraft_!.expirationMonth}">
            ${this.getMonthLabel_(month)}
          </option>`)}
      </select>
      <select class="popup" data-field="expirationYear"
          aria-label="Expiry year" @change="${this.onCardExpiryChange_}">
        ${this.years.map(year => html`
          <option value="${year}"
              ?selected="${year === this.cardDraft_!.expirationYear}">
            ${year}
          </option>`)}
      </select>
    </div>
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">Nickname</span>
        <span class="row-detail">Optional.</span>
      </span>
      <input class="field" data-field="nickname" aria-label="Nickname"
          autocomplete="off" .value="${this.cardDraft_.nickname}"
          @input="${this.onCardInput_}">
    </div>
  </div>
  <div class="actions">
    <span class="error" ?hidden="${!this.draftError_}">${this.draftError_}</span>
    <button class="pill" @click="${this.onCancelDraftClick_}">Cancel</button>
    <button class="pill accent" @click="${this.onSaveCardClick_}">Save</button>
  </div>` : nothing}
</section>` : nothing}
${this.isView_('addresses') ? html`
<button class="back" @click="${this.onBackClick_}">
  <compass-settings-icon name="back"></compass-settings-icon>
  Passwords
</button>
<h2 class="subpage-title">Addresses</h2>
<section class="row-group">
  <div class="group">
    ${this.addresses_.map(address => html`
      <div class="row control-row">
        <span class="tile" data-color="orange">
          <compass-settings-icon name="home"></compass-settings-icon>
        </span>
        <span class="row-text">
          <span class="row-label">${address.name}</span>
          <span class="row-detail">${address.summary}</span>
        </span>
        <button class="pill small" data-guid="${address.guid}"
            @click="${this.onEditAddressClick_}">
          Edit
        </button>
        <button class="pill small" data-guid="${address.guid}"
            @click="${this.onRemoveAddressClick_}">
          Remove
        </button>
      </div>`)}
    <p class="empty" ?hidden="${this.addresses_.length > 0}">
      No saved addresses.
    </p>
  </div>
  <div class="actions" ?hidden="${!!this.addressDraft_}">
    <button class="pill" data-row="addAddress"
        @click="${this.onAddAddressClick_}">
      Add Address…
    </button>
  </div>
  ${this.addressDraft_ ? html`
  <h2 class="group-title draft-title">${this.getAddressDraftTitle_()}</h2>
  <div class="group">
    <div class="row control-row">
      <span class="row-text"><span class="row-label">Country</span></span>
      <select class="popup" data-field="countryCode" aria-label="Country"
          @change="${this.onCountryChange_}">
        ${this.countries.map(country => html`
          <option value="${country.code}"
              ?selected="${country.code === this.addressDraft_!.countryCode}">
            ${country.name}
          </option>`)}
      </select>
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">Name</span></span>
      <input class="field" data-field="fullName" aria-label="Name"
          autocomplete="off" .value="${this.addressDraft_.fullName}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">Organization</span></span>
      <input class="field" data-field="organization" aria-label="Organization"
          autocomplete="off" .value="${this.addressDraft_.organization}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">Street</span></span>
      <textarea class="field" data-field="street" aria-label="Street" rows="2"
          .value="${this.addressDraft_.street}"
          @input="${this.onAddressInput_}"></textarea>
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">City</span></span>
      <input class="field" data-field="city" aria-label="City"
          autocomplete="off" .value="${this.addressDraft_.city}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">State, province or region</span>
      </span>
      <input class="field" data-field="state" aria-label="State"
          autocomplete="off" .value="${this.addressDraft_.state}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">Postal code</span></span>
      <input class="field" data-field="postalCode" aria-label="Postal code"
          autocomplete="off" .value="${this.addressDraft_.postalCode}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">Phone</span></span>
      <input class="field" data-field="phone" aria-label="Phone" type="tel"
          autocomplete="off" .value="${this.addressDraft_.phone}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">Email</span></span>
      <input class="field" data-field="email" aria-label="Email" type="email"
          autocomplete="off" .value="${this.addressDraft_.email}"
          @input="${this.onAddressInput_}">
    </div>
  </div>
  <div class="actions">
    <span class="error" ?hidden="${!this.draftError_}">${this.draftError_}</span>
    <button class="pill" @click="${this.onCancelDraftClick_}">Cancel</button>
    <button class="pill accent" @click="${this.onSaveAddressClick_}">
      Save
    </button>
  </div>` : nothing}
</section>` : nothing}
<!--_html_template_end_-->`;
  // clang-format on
}
