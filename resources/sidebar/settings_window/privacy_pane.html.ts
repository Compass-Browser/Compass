// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSettingsPrivacyPaneElement} from './privacy_pane.js';
import {PREF_ADBLOCK, PREF_CLEAN_LINKS, PREF_COOKIE_CONTROLS, PREF_DO_NOT_TRACK, PREF_HTTPS_ONLY, PREF_SECURE_DNS} from './privacy_pane.js';

export function getHtml(this: CompassSettingsPrivacyPaneElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.isView_('main') ? html`
<section class="row-group">
  <h2 class="group-title">Browsing data</h2>
  <div class="group">
    <button class="row" data-row="clearBrowserData" data-view="clear"
        @click="${this.onViewClick_}">
      <span class="row-text">
        <span class="row-label">Clear browsing data…</span>
        <span class="row-detail">
          History, cookies, cached images and files.
        </span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group" data-row="cookies">
  <h2 class="group-title">Cookies and tracking</h2>
  <div class="group">
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">Third-party cookies</span>
        <span class="row-detail">
          Whether sites can follow you across the web.
        </span>
      </span>
      <select class="popup" aria-label="Third-party cookies"
          ?disabled="${this.isEnforced_(PREF_COOKIE_CONTROLS)}"
          @change="${this.onCookieModeChange_}">
        ${this.cookieModes.map(mode => html`
          <option value="${mode.value}" ?selected="${
              mode.value === this.getNumber_(PREF_COOKIE_CONTROLS)}">
            ${mode.label}
          </option>`)}
      </select>
    </div>
    <label class="row control-row" data-row="doNotTrack">
      <span class="row-text">
        <span class="row-label">Ask sites not to track you</span>
        <span class="row-detail">Sends Do Not Track; sites may ignore it.</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_DO_NOT_TRACK}"
          .checked="${this.getBool_(PREF_DO_NOT_TRACK)}"
          ?disabled="${this.isEnforced_(PREF_DO_NOT_TRACK)}"
          @change="${this.onSwitchChange_}">
    </label>
    <label class="row control-row" data-row="cleanLinks">
      <span class="row-text">
        <span class="row-label">Remove tracking from copied links</span>
        <span class="row-detail">
          Copy URL and Copy Link leave out parameters that only track where a
          link was shared, using Firefox's list.
        </span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_CLEAN_LINKS}"
          .checked="${this.getBool_(PREF_CLEAN_LINKS)}"
          ?disabled="${this.isEnforced_(PREF_CLEAN_LINKS)}"
          @change="${this.onSwitchChange_}">
    </label>
  </div>
</section>

<section class="row-group" data-row="adblock">
  <h2 class="group-title">Ad blocker</h2>
  <div class="group">
    <label class="row control-row" data-row="adblockEnabled">
      <span class="row-text">
        <span class="row-label">Block ads and trackers</span>
        <span class="row-detail">
          On every site unless you turn it off for one, from the site's
          information or its permissions (Ads).
        </span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_ADBLOCK}"
          .checked="${this.getBool_(PREF_ADBLOCK)}"
          ?disabled="${this.isEnforced_(PREF_ADBLOCK)}"
          @change="${this.onSwitchChange_}">
    </label>
    <button class="row" data-row="filterLists" data-view="adblock"
        @click="${this.onViewClick_}">
      <span class="row-text">
        <span class="row-label">Filter lists</span>
        <span class="row-detail">${this.getFilterListsDetail_()}</span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group" data-row="security">
  <h2 class="group-title">Security</h2>
  <div class="group">
    <div class="row control-row" data-row="safeBrowsing">
      <span class="row-text">
        <span class="row-label">Safe Browsing</span>
        <span class="row-detail">
          Warns about dangerous sites, downloads and extensions.
        </span>
      </span>
      <select class="popup" aria-label="Safe Browsing"
          ?disabled="${this.isSafeBrowsingEnforced_()}"
          @change="${this.onSafeBrowsingChange_}">
        ${this.safeBrowsingLevels.map(level => html`
          <option value="${level.value}" ?selected="${
              level.value === this.getSafeBrowsingLevel_()}">
            ${level.label}
          </option>`)}
      </select>
    </div>
    <label class="row control-row" data-row="httpsOnly">
      <span class="row-text">
        <span class="row-label">Always use secure connections</span>
        <span class="row-detail">
          Asks before loading a site that doesn't support HTTPS.
        </span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_HTTPS_ONLY}"
          .checked="${this.getBool_(PREF_HTTPS_ONLY)}"
          ?disabled="${this.isEnforced_(PREF_HTTPS_ONLY)}"
          @change="${this.onSwitchChange_}">
    </label>
    <label class="row control-row" data-row="secureDns">
      <span class="row-text">
        <span class="row-label">Use secure DNS</span>
        <span class="row-detail">${this.getSecureDnsDetail_()}</span>
      </span>
      <input type="checkbox" class="switch"
          .checked="${this.getSecureDnsMode_() !== 'off'}"
          ?disabled="${this.isEnforced_(PREF_SECURE_DNS)}"
          @change="${this.onSecureDnsChange_}">
    </label>
    <button class="row" data-row="certificates"
        @click="${this.onCertificatesClick_}">
      <span class="row-text">
        <span class="row-label">Manage certificates</span>
      </span>
      <compass-settings-icon class="row-trail" name="external"
          title="Opens in a tab">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">Site settings</h2>
  <div class="group">
    <button class="row" data-row="permissions" data-view="permissions"
        @click="${this.onViewClick_}">
      <span class="row-text">
        <span class="row-label">Permissions</span>
        <span class="row-detail">
          What sites can use by default: location, camera, notifications…
        </span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
    <button class="row" data-row="sites" data-view="sites"
        @click="${this.onViewClick_}">
      <span class="row-text">
        <span class="row-label">Sites with their own permissions</span>
        <span class="row-detail">${this.getSitesDetail_()}</span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
    <button class="row" data-row="safetyCheck" data-view="safety"
        @click="${this.onViewClick_}">
      <span class="row-text">
        <span class="row-label">Safety check</span>
        <span class="row-detail">
          Updates, passwords, Safe Browsing and extensions.
        </span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>` : nothing}

${!this.isView_('main') ? html`
<button class="back" @click="${this.onBackClick_}">
  <compass-settings-icon name="back"></compass-settings-icon>
  ${this.getBackLabel_()}
</button>` : nothing}

${this.isView_('safety') ? html`
<section class="row-group">
  <h2 class="group-title">Safety check</h2>
  <div class="group">
    <div class="row control-row" data-check="updates">
      <span class="status" ?ok="${this.isUpdatesOk_()}"></span>
      <span class="row-text">
        <span class="row-label">Updates</span>
        <span class="row-detail">${this.getUpdatesText_()}</span>
      </span>
      <button class="pill small" ?hidden="${!this.canUpdate_}"
          @click="${this.onCheckUpdatesClick_}">
        Check Now
      </button>
    </div>
    <div class="row control-row" data-check="passwords">
      <span class="status" ?ok="${this.isPasswordsOk_()}"></span>
      <span class="row-text">
        <span class="row-label">Passwords</span>
        <span class="row-detail">${this.getPasswordsText_()}</span>
        <span class="row-detail">
          Whether they appeared in a data breach can't be checked online
          without a Google account.
        </span>
      </span>
      <button class="pill small" @click="${this.onReviewPasswordsClick_}">
        Review
      </button>
    </div>
    <div class="row control-row" data-check="safeBrowsing">
      <span class="status" ?ok="${this.getSafeBrowsingLevel_() > 0}"></span>
      <span class="row-text">
        <span class="row-label">Safe Browsing</span>
        <span class="row-detail">${this.getSafeBrowsingText_()}</span>
      </span>
      <button class="pill small accent"
          ?hidden="${this.getSafeBrowsingLevel_() > 0}"
          ?disabled="${this.isSafeBrowsingEnforced_()}"
          @click="${this.onTurnOnSafeBrowsingClick_}">
        Turn On
      </button>
    </div>
    <div class="row control-row" data-check="extensions">
      <span class="status" ?ok="${this.isExtensionsOk_()}"></span>
      <span class="row-text">
        <span class="row-label">Extensions</span>
        <span class="row-detail">${this.getExtensionsText_()}</span>
      </span>
      <button class="pill small" @click="${this.onManageExtensionsClick_}">
        Manage
      </button>
    </div>
    <button class="row" data-view="sites" @click="${this.onViewClick_}">
      <span class="status" ok></span>
      <span class="row-text">
        <span class="row-label">Site permissions</span>
        <span class="row-detail">${this.getSitesDetail_()} with their own.</span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>` : nothing}

${this.isView_('adblock') ? html`
<section class="row-group">
  <div class="group-header">
    <h2 class="group-title">Filter lists</h2>
    <button class="pill small" ?disabled="${this.isUpdatingLists_()}"
        @click="${this.onUpdateFilterListsClick_}">
      ${this.isUpdatingLists_() ? 'Updating…' : 'Update Now'}
    </button>
  </div>
  <div class="group">
    ${this.filterLists_.map(list => html`
      <div class="row control-row" data-list="${list.id}">
        <input type="checkbox" class="switch" data-id="${list.id}"
            aria-label="Use ${list.title}" .checked="${list.enabled}"
            @change="${this.onFilterListChange_}">
        <span class="row-text">
          <span class="row-label">${list.title}</span>
          <span class="row-detail">${this.getFilterListDetail_(list)}</span>
          <span class="row-detail error"
              ?hidden="${!list.error || !list.updatedMs}">
            ${list.error}
          </span>
        </span>
        <button class="pill small" data-id="${list.id}"
            ?hidden="${list.builtin}" @click="${this.onRemoveFilterListClick_}">
          Remove
        </button>
      </div>`)}
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">Add a list</span>
        <span class="row-detail">
          The address of an Adblock Plus or uBlock Origin list.
        </span>
      </span>
      <input class="field" type="url" aria-label="List address"
          spellcheck="false" placeholder="https://"
          .value="${this.listUrl_}" @input="${this.onListUrlInput_}"
          @keydown="${this.onListUrlKeydown_}">
      <button class="pill small" ?disabled="${!this.listUrl_.trim()}"
          @click="${this.onAddFilterListClick_}">
        Add
      </button>
    </div>
  </div>
  <div class="actions" ?hidden="${!this.listError_}">
    <span class="error">${this.listError_}</span>
  </div>
  <p class="group-footer">
    Lists are downloaded from their publishers and checked for updates once
    a day. Scriptlets and some uBlock Origin rules aren't supported.
  </p>
</section>` : nothing}

${this.isView_('clear') ? html`
<section class="row-group">
  <h2 class="group-title">Clear browsing data</h2>
  <div class="group">
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">Time range</span>
      </span>
      <select class="popup" aria-label="Time range"
          @change="${this.onClearPeriodChange_}">
        ${this.timePeriods.map(period => html`
          <option value="${period.value}"
              ?selected="${period.value === this.clearPeriod_}">
            ${period.label}
          </option>`)}
      </select>
    </div>
    ${this.clearChoices.map(choice => html`
      <label class="row control-row">
        <input type="checkbox" class="check" data-key="${choice.key}"
            .checked="${this.isClearChecked_(choice)}"
            @change="${this.onClearChoiceChange_}">
        <span class="row-text">
          <span class="row-label">${choice.label}</span>
          <span class="row-detail">${choice.detail}</span>
        </span>
      </label>`)}
  </div>
  <div class="actions">
    <button class="pill accent" ?disabled="${!this.canClear_()}"
        @click="${this.onClearClick_}">
      ${this.getClearButtonText_()}
    </button>
  </div>
</section>` : nothing}

${this.isView_('permissions') ? html`
<section class="row-group">
  <h2 class="group-title">Permissions</h2>
  <div class="group">
    ${this.defaults_.map(permission => html`
      <div class="row control-row" data-row="${permission.type}">
        <span class="row-text">
          <span class="row-label">
            ${this.getPermissionLabel_(permission.type)}
          </span>
        </span>
        <select class="popup"
            aria-label="${this.getPermissionLabel_(permission.type)}"
            data-type="${permission.type}" ?disabled="${permission.managed}"
            @change="${this.onDefaultChange_}">
          ${permission.choices.map(value => html`
            <option value="${value}" ?selected="${value === permission.value}">
              ${this.getValueLabel_(value)}
            </option>`)}
        </select>
      </div>`)}
  </div>
</section>` : nothing}

${this.isView_('sites') ? html`
<section class="row-group">
  <h2 class="group-title">Sites with their own permissions</h2>
  <div class="group">
    ${this.sites_.map(site => html`
      <button class="row" data-origin="${site.origin}"
          @click="${this.onSiteClick_}">
        <span class="row-text">
          <span class="row-label">${site.displayName}</span>
          <span class="row-detail">${this.getPermissionCountText_(site)}</span>
        </span>
        <compass-settings-icon class="row-trail" name="chevron">
        </compass-settings-icon>
      </button>`)}
    <p class="empty" ?hidden="${this.sites_.length > 0}">
      No site has permissions of its own.
    </p>
  </div>
</section>` : nothing}

${this.isView_('site') ? html`
<section class="row-group">
  <h2 class="group-title">${this.siteName_}</h2>
  <div class="group">
    ${this.sitePermissions_.map(permission => html`
      <div class="row control-row" data-row="${permission.type}">
        <span class="row-text">
          <span class="row-label">
            ${this.getPermissionLabel_(permission.type)}
          </span>
        </span>
        <select class="popup"
            aria-label="${this.getPermissionLabel_(permission.type)}"
            data-type="${permission.type}" ?disabled="${permission.managed}"
            @change="${this.onSitePermissionChange_}">
          ${this.getSiteChoices_(permission).map(value => html`
            <option value="${value}" ?selected="${value === permission.value}">
              ${this.getSiteValueLabel_(permission, value)}
            </option>`)}
        </select>
      </div>`)}
  </div>
  <div class="actions">
    <button class="pill" @click="${this.onResetSiteClick_}">
      Reset Permissions
    </button>
    <button class="pill" ?disabled="${this.siteDataCleared_}"
        @click="${this.onClearSiteDataClick_}">
      ${this.getSiteDataButtonText_()}
    </button>
  </div>
</section>` : nothing}
<!--_html_template_end_-->`;
  // clang-format on
}
