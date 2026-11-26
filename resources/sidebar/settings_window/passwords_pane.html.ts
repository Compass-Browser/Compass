// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n, i18nF} from '../i18n.js';

import type {CompassSettingsPasswordsPaneElement} from './passwords_pane.js';
import {PREF_AUTO_SIGN_IN, PREF_OFFER_TO_SAVE, PREF_SAVE_ADDRESSES, PREF_SAVE_CARDS} from './passwords_pane.js';

export function getHtml(this: CompassSettingsPasswordsPaneElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.isView_('main') ? html`
<section class="row-group">
  <h2 class="group-title">${i18n('settingsWindowPanePasswords')}</h2>
  <div class="group">
    <label class="row control-row" data-row="offerToSave">
      <span class="tile" data-color="green">
        <compass-settings-icon name="passwords"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('passwordsPaneOfferToSave')}</span>
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
        <span class="row-label">${i18n('passwordsPaneAutoSignIn')}</span>
        <span class="row-detail">
          ${i18n('passwordsPaneAutoSignInDetail')}
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
        <span class="row-label">${i18n('passwordsPaneSavedPasswords')}</span>
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
        <span class="row-label">${i18n('passwordsPanePasswordManager')}</span>
        <span class="row-detail">
          ${i18n('passwordsPanePasswordManagerDetail')}
        </span>
      </span>
      <compass-settings-icon class="row-trail" name="external"
          title="${i18n('settingsWindowOpensInTab')}">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">${i18n('passwordsPanePaymentMethods')}</h2>
  <div class="group">
    <label class="row control-row" data-row="saveCards">
      <span class="tile" data-color="green">
        <compass-settings-icon name="card"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('passwordsPaneSaveCards')}</span>
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
        <span class="row-label">${i18n('passwordsPaneCards')}</span>
      </span>
      <span class="row-count">${this.cards_.length}</span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">${i18n('passwordsPaneAddresses')}</h2>
  <div class="group">
    <label class="row control-row" data-row="saveAddresses">
      <span class="tile" data-color="green">
        <compass-settings-icon name="home"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('passwordsPaneSaveAddresses')}</span>
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
        <span class="row-label">${i18n('passwordsPaneAddresses')}</span>
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
  ${i18n('settingsWindowPanePasswords')}
</button>
<h2 class="subpage-title">${i18n('passwordsPaneSavedPasswords')}</h2>
<section class="row-group" data-row="savedPasswords">
  <div class="group-header">
    <h2 class="group-title">${this.getPasswordsTitle_()}</h2>
    <input class="field filter" type="search" placeholder="${i18n('settingsWindowFilter')}"
        aria-label="${i18n('passwordsPaneFilterLabel')}" spellcheck="false"
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
            aria-label="${i18nF('passwordsPaneRemovePassword', password.site)}"
            @click="${this.onRemovePasswordClick_}">
          ${i18n('remove')}
        </button>
      </div>`)}
    <p class="empty" ?hidden="${this.getShownPasswords_().length > 0}">
      ${i18n('passwordsPaneNoPasswords')}
    </p>
  </div>
</section>` : nothing}
${this.isView_('cards') ? html`
<button class="back" @click="${this.onBackClick_}">
  <compass-settings-icon name="back"></compass-settings-icon>
  ${i18n('settingsWindowPanePasswords')}
</button>
<h2 class="subpage-title">${i18n('passwordsPaneCards')}</h2>
<section class="row-group">
  <div class="group">
    ${this.cards_.map(card => html`
      <div class="row control-row">
        <span class="tile" data-color="indigo">
          <compass-settings-icon name="card"></compass-settings-icon>
        </span>
        <span class="row-text">
          <span class="row-label">${this.getCardText_(card)}</span>
          <span class="row-detail">
            ${i18nF('passwordsPaneCardExpires', card.expiration)}
          </span>
        </span>
        <button class="pill small" data-guid="${card.guid}"
            ?hidden="${!card.removable}" @click="${this.onEditCardClick_}">
          ${i18n('edit')}
        </button>
        <button class="pill small" data-guid="${card.guid}"
            ?hidden="${!card.removable}" @click="${this.onRemoveCardClick_}">
          ${i18n('remove')}
        </button>
      </div>`)}
    <p class="empty" ?hidden="${this.cards_.length > 0}">
    ${i18n('passwordsPaneNoCards')}
  </p>
  </div>
  <div class="actions" ?hidden="${!!this.cardDraft_}">
    <button class="pill" data-row="addCard"
        @click="${this.onAddCardClick_}">
      ${i18n('passwordsPaneAddCardEllipsis')}
    </button>
  </div>
  ${this.cardDraft_ ? html`
  <h2 class="group-title draft-title">${this.getCardDraftTitle_()}</h2>
  <div class="group">
    <div class="row control-row">
      <span class="row-text"><span class="row-label">${i18n('passwordsPaneNameOnCard')}</span></span>
      <input class="field" data-field="nameOnCard" aria-label="${i18n('passwordsPaneNameOnCard')}"
          autocomplete="off" .value="${this.cardDraft_.nameOnCard}"
          @input="${this.onCardInput_}">
    </div>
    <div class="row control-row" ?hidden="${!this.isNewCard_()}">
      <span class="row-text"><span class="row-label">${i18n('passwordsPaneCardNumber')}</span></span>
      <input class="field" data-field="number" aria-label="${i18n('passwordsPaneCardNumber')}"
          inputmode="numeric" autocomplete="off"
          .value="${this.cardDraft_.number}" @input="${this.onCardInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">${i18n('passwordsPaneExpires')}</span></span>
      <select class="popup" data-field="expirationMonth"
          aria-label="${i18n('passwordsPaneExpiryMonth')}" @change="${this.onCardExpiryChange_}">
        ${this.months.map(month => html`
          <option value="${month}"
              ?selected="${month === this.cardDraft_!.expirationMonth}">
            ${this.getMonthLabel_(month)}
          </option>`)}
      </select>
      <select class="popup" data-field="expirationYear"
          aria-label="${i18n('passwordsPaneExpiryYear')}" @change="${this.onCardExpiryChange_}">
        ${this.years.map(year => html`
          <option value="${year}"
              ?selected="${year === this.cardDraft_!.expirationYear}">
            ${year}
          </option>`)}
      </select>
    </div>
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">${i18n('passwordsPaneNickname')}</span>
        <span class="row-detail">${i18n('passwordsPaneOptional')}</span>
      </span>
      <input class="field" data-field="nickname" aria-label="${i18n('passwordsPaneNickname')}"
          autocomplete="off" .value="${this.cardDraft_.nickname}"
          @input="${this.onCardInput_}">
    </div>
  </div>
  <div class="actions">
    <span class="error" ?hidden="${!this.draftError_}">${this.draftError_}</span>
    <button class="pill" @click="${this.onCancelDraftClick_}">
      ${i18n('cancel')}
    </button>
    <button class="pill accent" @click="${this.onSaveCardClick_}">
      ${i18n('save')}
    </button>
  </div>` : nothing}
</section>` : nothing}
${this.isView_('addresses') ? html`
<button class="back" @click="${this.onBackClick_}">
  <compass-settings-icon name="back"></compass-settings-icon>
  ${i18n('settingsWindowPanePasswords')}
</button>
<h2 class="subpage-title">${i18n('passwordsPaneAddresses')}</h2>
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
          ${i18n('edit')}
        </button>
        <button class="pill small" data-guid="${address.guid}"
            @click="${this.onRemoveAddressClick_}">
          ${i18n('remove')}
        </button>
      </div>`)}
    <p class="empty" ?hidden="${this.addresses_.length > 0}">
      ${i18n('passwordsPaneNoAddresses')}
    </p>
  </div>
  <div class="actions" ?hidden="${!!this.addressDraft_}">
    <button class="pill" data-row="addAddress"
        @click="${this.onAddAddressClick_}">
      ${i18n('passwordsPaneAddAddressEllipsis')}
    </button>
  </div>
  ${this.addressDraft_ ? html`
  <h2 class="group-title draft-title">${this.getAddressDraftTitle_()}</h2>
  <div class="group">
    <div class="row control-row">
      <span class="row-text"><span class="row-label">${i18n('passwordsPaneCountry')}</span></span>
      <select class="popup" data-field="countryCode" aria-label="${i18n('passwordsPaneCountry')}"
          @change="${this.onCountryChange_}">
        ${this.countries.map(country => html`
          <option value="${country.code}"
              ?selected="${country.code === this.addressDraft_!.countryCode}">
            ${country.name}
          </option>`)}
      </select>
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">${i18n('passwordsPaneName')}</span></span>
      <input class="field" data-field="fullName" aria-label="${i18n('passwordsPaneName')}"
          autocomplete="off" .value="${this.addressDraft_.fullName}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">${i18n('passwordsPaneOrganization')}</span></span>
      <input class="field" data-field="organization" aria-label="${i18n('passwordsPaneOrganization')}"
          autocomplete="off" .value="${this.addressDraft_.organization}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">${i18n('passwordsPaneStreet')}</span></span>
      <textarea class="field" data-field="street" aria-label="${i18n('passwordsPaneStreet')}" rows="2"
          .value="${this.addressDraft_.street}"
          @input="${this.onAddressInput_}"></textarea>
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">${i18n('passwordsPaneCity')}</span></span>
      <input class="field" data-field="city" aria-label="${i18n('passwordsPaneCity')}"
          autocomplete="off" .value="${this.addressDraft_.city}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">${i18n('passwordsPaneState')}</span>
      </span>
      <input class="field" data-field="state" aria-label="${i18n('passwordsPaneState')}"
          autocomplete="off" .value="${this.addressDraft_.state}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">${i18n('passwordsPanePostalCode')}</span></span>
      <input class="field" data-field="postalCode" aria-label="${i18n('passwordsPanePostalCode')}"
          autocomplete="off" .value="${this.addressDraft_.postalCode}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">${i18n('passwordsPanePhone')}</span></span>
      <input class="field" data-field="phone" aria-label="${i18n('passwordsPanePhone')}" type="tel"
          autocomplete="off" .value="${this.addressDraft_.phone}"
          @input="${this.onAddressInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text"><span class="row-label">${i18n('passwordsPaneEmail')}</span></span>
      <input class="field" data-field="email" aria-label="${i18n('passwordsPaneEmail')}" type="email"
          autocomplete="off" .value="${this.addressDraft_.email}"
          @input="${this.onAddressInput_}">
    </div>
  </div>
  <div class="actions">
    <span class="error" ?hidden="${!this.draftError_}">${this.draftError_}</span>
    <button class="pill" @click="${this.onCancelDraftClick_}">
      ${i18n('cancel')}
    </button>
    <button class="pill accent" @click="${this.onSaveAddressClick_}">
      ${i18n('save')}
    </button>
  </div>` : nothing}
</section>` : nothing}
<!--_html_template_end_-->`;
  // clang-format on
}
