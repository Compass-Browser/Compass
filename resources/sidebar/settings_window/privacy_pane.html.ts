// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n, i18nF} from '../i18n.js';

import type {CompassSettingsPrivacyPaneElement} from './privacy_pane.js';
import {PREF_ADBLOCK, PREF_CLEAN_LINKS, PREF_COOKIE_CONTROLS, PREF_DO_NOT_TRACK, PREF_HTTPS_ONLY, PREF_SECURE_DNS} from './privacy_pane.js';

export function getHtml(this: CompassSettingsPrivacyPaneElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.isView_('main') ? html`
<section class="row-group">
  <h2 class="group-title">${i18n('privacyPaneBrowsingData')}</h2>
  <div class="group">
    <button class="row" data-row="clearBrowserData" data-view="clear"
        @click="${this.onViewClick_}">
      <span class="tile" data-color="red">
        <compass-settings-icon name="trash"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneClearBrowsingDataEllipsis')}</span>
        <span class="row-detail">
          ${i18n('privacyPaneClearBrowsingDataDetail')}
        </span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group" data-row="cookies">
  <h2 class="group-title">${i18n('privacyPaneCookiesAndTracking')}</h2>
  <div class="group">
    <div class="row control-row">
      <span class="tile" data-color="orange">
        <compass-settings-icon name="cookie"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneThirdPartyCookies')}</span>
        <span class="row-detail">
          ${i18n('privacyPaneThirdPartyCookiesDetail')}
        </span>
      </span>
      <select class="popup" aria-label="${i18n('privacyPaneThirdPartyCookies')}"
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
      <span class="tile" data-color="slate">
        <compass-settings-icon name="eyeoff"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneDoNotTrack')}</span>
        <span class="row-detail">${i18n('privacyPaneDoNotTrackDetail')}</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_DO_NOT_TRACK}"
          .checked="${this.getBool_(PREF_DO_NOT_TRACK)}"
          ?disabled="${this.isEnforced_(PREF_DO_NOT_TRACK)}"
          @change="${this.onSwitchChange_}">
    </label>
    <label class="row control-row" data-row="cleanLinks">
      <span class="tile" data-color="teal">
        <compass-settings-icon name="links"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneCleanLinks')}</span>
        <span class="row-detail">
          ${i18n('privacyPaneCleanLinksDetail')}
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
  <h2 class="group-title">${i18n('privacyPaneAdBlocker')}</h2>
  <div class="group">
    <label class="row control-row" data-row="adblockEnabled">
      <span class="tile" data-color="red">
        <compass-settings-icon name="privacy"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneBlockAds')}</span>
        <span class="row-detail">
          ${i18n('privacyPaneBlockAdsDetail')}
        </span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_ADBLOCK}"
          .checked="${this.getBool_(PREF_ADBLOCK)}"
          ?disabled="${this.isEnforced_(PREF_ADBLOCK)}"
          @change="${this.onSwitchChange_}">
    </label>
    <button class="row" data-row="filterLists" data-view="adblock"
        @click="${this.onViewClick_}">
      <span class="tile" data-color="gray">
        <compass-settings-icon name="filter"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneFilterLists')}</span>
        <span class="row-detail">${this.getFilterListsDetail_()}</span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group" data-row="security">
  <h2 class="group-title">${i18n('privacyPaneSecurity')}</h2>
  <div class="group">
    <div class="row control-row" data-row="safeBrowsing">
      <span class="tile" data-color="blue">
        <compass-settings-icon name="privacy"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneSafeBrowsing')}</span>
        <span class="row-detail">
          ${i18n('privacyPaneSafeBrowsingDetail')}
        </span>
      </span>
      <select class="popup" aria-label="${i18n('privacyPaneSafeBrowsing')}"
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
      <span class="tile" data-color="green">
        <compass-settings-icon name="lock"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneHttpsOnly')}</span>
        <span class="row-detail">
          ${i18n('privacyPaneHttpsOnlyDetail')}
        </span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_HTTPS_ONLY}"
          .checked="${this.getBool_(PREF_HTTPS_ONLY)}"
          ?disabled="${this.isEnforced_(PREF_HTTPS_ONLY)}"
          @change="${this.onSwitchChange_}">
    </label>
    <label class="row control-row" data-row="secureDns">
      <span class="tile" data-color="indigo">
        <compass-settings-icon name="server"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneSecureDns')}</span>
        <span class="row-detail">${this.getSecureDnsDetail_()}</span>
      </span>
      <input type="checkbox" class="switch"
          .checked="${this.getSecureDnsMode_() !== 'off'}"
          ?disabled="${this.isEnforced_(PREF_SECURE_DNS)}"
          @change="${this.onSecureDnsChange_}">
    </label>
    <button class="row" data-row="certificates"
        @click="${this.onCertificatesClick_}">
      <span class="tile" data-color="gray">
        <compass-settings-icon name="doc"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneManageCertificates')}</span>
      </span>
      <compass-settings-icon class="row-trail" name="external"
          title="${i18n('settingsWindowOpensInTab')}">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">${i18n('privacyPaneSiteSettings')}</h2>
  <div class="group">
    <button class="row" data-row="permissions" data-view="permissions"
        @click="${this.onViewClick_}">
      <span class="tile" data-color="purple">
        <compass-settings-icon name="advanced"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPanePermissions')}</span>
        <span class="row-detail">
          ${i18n('privacyPanePermissionsDetail')}
        </span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
    <button class="row" data-row="sites" data-view="sites"
        @click="${this.onViewClick_}">
      <span class="tile" data-color="teal">
        <compass-settings-icon name="globe"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneSitesWithPermissions')}</span>
        <span class="row-detail">${this.getSitesDetail_()}</span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
    <button class="row" data-row="safetyCheck" data-view="safety"
        @click="${this.onViewClick_}">
      <span class="tile" data-color="green">
        <compass-settings-icon name="check"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneSafetyCheck')}</span>
        <span class="row-detail">
          ${i18n('privacyPaneSafetyCheckDetail')}
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
  <h2 class="group-title">${i18n('privacyPaneSafetyCheck')}</h2>
  <div class="group">
    <div class="row control-row" data-check="updates">
      <span class="status" ?ok="${this.isUpdatesOk_()}"></span>
      <span class="row-text">
        <span class="row-label">${i18n('aboutPaneUpdates')}</span>
        <span class="row-detail">${this.getUpdatesText_()}</span>
      </span>
      <button class="pill small" ?hidden="${!this.canUpdate_}"
          @click="${this.onCheckUpdatesClick_}">
        ${i18n('privacyPaneCheckNow')}
      </button>
    </div>
    <div class="row control-row" data-check="passwords">
      <span class="status" ?ok="${this.isPasswordsOk_()}"></span>
      <span class="row-text">
        <span class="row-label">${i18n('settingsWindowPanePasswords')}</span>
        <span class="row-detail">${this.getPasswordsText_()}</span>
        <span class="row-detail">
          ${i18n('privacyPaneBreachCheckUnavailable')}
        </span>
      </span>
      <button class="pill small" @click="${this.onReviewPasswordsClick_}">
        ${i18n('privacyPaneReview')}
      </button>
    </div>
    <div class="row control-row" data-check="safeBrowsing">
      <span class="status" ?ok="${this.getSafeBrowsingLevel_() > 0}"></span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneSafeBrowsing')}</span>
        <span class="row-detail">${this.getSafeBrowsingText_()}</span>
      </span>
      <button class="pill small accent"
          ?hidden="${this.getSafeBrowsingLevel_() > 0}"
          ?disabled="${this.isSafeBrowsingEnforced_()}"
          @click="${this.onTurnOnSafeBrowsingClick_}">
        ${i18n('privacyPaneTurnOn')}
      </button>
    </div>
    <div class="row control-row" data-check="extensions">
      <span class="status" ?ok="${this.isExtensionsOk_()}"></span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneExtensions')}</span>
        <span class="row-detail">${this.getExtensionsText_()}</span>
      </span>
      <button class="pill small" @click="${this.onManageExtensionsClick_}">
        ${i18n('privacyPaneManage')}
      </button>
    </div>
    <button class="row" data-view="sites" @click="${this.onViewClick_}">
      <span class="status" ok></span>
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneSitePermissions')}</span>
        <span class="row-detail">${this.getSitesWithOwnText_()}</span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>` : nothing}

${this.isView_('adblock') ? html`
<section class="row-group">
  <div class="group-header">
    <h2 class="group-title">${i18n('privacyPaneFilterLists')}</h2>
    <button class="pill small" ?disabled="${this.isUpdatingLists_()}"
        @click="${this.onUpdateFilterListsClick_}">
      ${this.isUpdatingLists_() ? i18n('privacyPaneUpdating') :
                                  i18n('privacyPaneUpdateNow')}
    </button>
  </div>
  <div class="group">
    ${this.filterLists_.map(list => html`
      <div class="row control-row" data-list="${list.id}">
        <input type="checkbox" class="switch" data-id="${list.id}"
            aria-label="${i18nF('privacyPaneUseList', list.title)}" .checked="${list.enabled}"
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
          ${i18n('remove')}
        </button>
      </div>`)}
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneAddList')}</span>
        <span class="row-detail">
          ${i18n('privacyPaneAddListDetail')}
        </span>
      </span>
      <input class="field" type="url" aria-label="${i18n('privacyPaneListAddress')}"
          spellcheck="false" placeholder="https://"
          .value="${this.listUrl_}" @input="${this.onListUrlInput_}"
          @keydown="${this.onListUrlKeydown_}">
      <button class="pill small" ?disabled="${!this.listUrl_.trim()}"
          @click="${this.onAddFilterListClick_}">
        ${i18n('add')}
      </button>
    </div>
  </div>
  <div class="actions" ?hidden="${!this.listError_}">
    <span class="error">${this.listError_}</span>
  </div>
  <p class="group-footer">
    ${i18n('privacyPaneFilterListsFooter')}
  </p>
</section>` : nothing}

${this.isView_('clear') ? html`
<section class="row-group">
  <h2 class="group-title">${i18n('privacyPaneClearBrowsingData')}</h2>
  <div class="group">
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">${i18n('privacyPaneTimeRange')}</span>
      </span>
      <select class="popup" aria-label="${i18n('privacyPaneTimeRange')}"
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
  <h2 class="group-title">${i18n('privacyPanePermissions')}</h2>
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
  <h2 class="group-title">${i18n('privacyPaneSitesWithPermissions')}</h2>
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
      ${i18n('privacyPaneNoSites')}
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
      ${i18n('privacyPaneResetPermissions')}
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
