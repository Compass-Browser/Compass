// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../settings_window/icon.js';

import {i18n, i18nF, i18nPlural} from '../i18n.js';
import type {ArcImportProfile, ArcImportSpace, ArcImportState, BrowserImportChoice, BrowserImportProfile, BrowserImportSummary} from '../sidebar.mojom-webui.js';
import {ArcImportHandler} from '../sidebar.mojom-webui.js';

import {getTemplate} from './arc_import_page.html.js';
import {CompassSettingsPage} from './page_base.js';

// Importing from another browser: compass://settings/importArc (a page of
// its own in the settings window), and alone on chrome://compass-import.
// Three views (docs/design/browser-import.md, "The page"):
// - the list: the browsers' profiles found on this computer, each with its
//   browser's own icon, and Arc when it's there;
// - a browser profile's choices (?browser=<id>): a space here or a Compass
//   profile of its own (Chromium-based ones), what to bring, and once
//   imported, what came over;
// - Arc's profiles (?view=arc, docs/design/arc-import.md).
// One browser at a time: "Import Another Browser" goes back to the list.

type View = 'list'|'browser'|'arc';

// macOS asks before Compass reads another browser's Keychain key.
function isMac(): boolean {
  return navigator.userAgent.includes('Mac');
}

function describe(profile: ArcImportProfile): string {
  return `${i18nPlural('arcImportPageSpaceCount', profile.spaces.length)} · ${
      i18nPlural('arcImportPageTabCount', profile.tabCount)}`;
}

// A letter tile's color for a browser without an icon (window.css, .tile).
const LETTER_COLORS = [
  'blue', 'orange', 'teal', 'purple', 'green', 'pink', 'indigo', 'red',
];

function letterColor(name: string): string {
  let hash = 0;
  for (const char of name) {
    hash = (hash * 31 + char.charCodeAt(0)) >>> 0;
  }
  return LETTER_COLORS[hash % LETTER_COLORS.length]!;
}

function element<K extends keyof HTMLElementTagNameMap>(
    tag: K, className = '', text = ''): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className) {
    node.className = className;
  }
  if (text) {
    node.textContent = text;
  }
  return node;
}

// A browser's own icon, or the first letter of its name on a color.
function browserIcon(name: string, iconUrl: string, large = false):
    HTMLElement {
  if (iconUrl) {
    const image = element('img', 'app-icon');
    image.src = iconUrl;
    image.alt = '';
    return image;
  }
  const tile = element(
      'span', large ? 'tile letter large' : 'tile letter', name.charAt(0));
  tile.dataset['color'] = letterColor(name);
  tile.setAttribute('aria-hidden', 'true');
  return tile;
}

// A white glyph on a color, a row's first item.
function tile(icon: string, color: string): HTMLElement {
  const span = element('span', 'tile');
  span.dataset['color'] = color;
  const glyph = document.createElement('compass-settings-icon');
  glyph.setAttribute('name', icon);
  span.append(glyph);
  return span;
}

function rowText(label: string, ...details: string[]): HTMLElement {
  const text = element('span', 'row-text');
  text.append(element('span', 'row-label', label));
  for (const detail of details) {
    if (detail) {
      text.append(element('span', 'row-detail', detail));
    }
  }
  return text;
}

function chevron(): HTMLElement {
  const trail = document.createElement('compass-settings-icon');
  trail.className = 'row-trail';
  trail.setAttribute('name', 'chevron');
  return trail;
}

// "Chrome · Work", or "Chrome".
function profileLabel(profile: BrowserImportProfile): string {
  return profile.name ? `${profile.browser} \u00b7 ${profile.name}` :
                        profile.browser;
}

// "Chrome, Firefox and Safari", in the page's language.
function listNames(names: string[]): string {
  return new Intl.ListFormat(i18n('language'), {type: 'conjunction'})
      .format(names);
}

function checkbox(className: string, value: string): HTMLInputElement {
  const input = document.createElement('input');
  input.type = 'checkbox';
  input.className = className;
  input.value = value;
  return input;
}

export class CompassArcImportPageElement extends CompassSettingsPage {
  private handler_ = ArcImportHandler.getRemote();
  private state_: ArcImportState|null = null;
  private browsers_: BrowserImportProfile[] = [];
  private browsersLoaded_ = false;
  // The profile the browser view shows.
  private chosen_: BrowserImportProfile|null = null;

  protected get pageTitle() {
    return i18n('arcImportPageTitle');
  }

  protected getPageTemplate() {
    return getTemplate();
  }

  protected setUp() {
    this.$('quitArc').addEventListener('click', async () => {
      this.$<HTMLButtonElement>('quitArc').disabled = true;
      await this.handler_.quitArc();
      this.$<HTMLButtonElement>('quitArc').disabled = false;
      await this.refresh_();
    });

    this.$('import').addEventListener('click', () => this.import_());
    this.$('open').addEventListener('click', () => this.handler_.openImported());
    this.$('importEasels').addEventListener('click', () => this.importEasels_());

    this.$('importBrowsers')
        .addEventListener('click', () => this.importBrowsers_());
    this.$('browsersRefresh')
        .addEventListener('click', () => this.refreshBrowsers_());
    this.$('openBrowsers')
        .addEventListener('click', () => this.handler_.openImported());
    this.$('browsersAgain').addEventListener('click', () => {
      this.setView_('list');
      this.refreshBrowsers_();
    });
    for (const id of ['browserBack', 'browserCancel', 'arcBack']) {
      this.$(id).addEventListener('click', () => this.setView_('list'));
    }
    this.$('arcRow').addEventListener('click', () => this.setView_('arc'));

    // The path picks a view: ?browser=<id> once the browsers are read.
    const params = new URLSearchParams(window.location.search);
    if (params.get('view') === 'arc') {
      this.setView_('arc', /*updatePath=*/ false);
    } else {
      this.setView_('list', /*updatePath=*/ false);
    }
    this.refreshBrowsers_().then(() => {
      const id = params.get('browser');
      const profile = id && this.browsers_.find(p => p.id === id);
      if (profile && this.isAvailable_(profile)) {
        this.openBrowser_(profile, /*updatePath=*/ false);
      }
    });
    this.refresh_();
  }

  // Views -------------------------------------------------------------------

  private setView_(view: View, updatePath = true) {
    this.show('listView', view === 'list');
    this.show('browserView', view === 'browser');
    this.show('arcView', view === 'arc');
    // The settings window hides its own way back and heading meanwhile
    // (settings_window/app.ts): the view draws its own.
    this.toggleAttribute('nested', view !== 'list');
    this.dispatchEvent(new CustomEvent('settings-view', {bubbles: true}));
    if (!updatePath) {
      return;
    }
    const url = new URL(window.location.href);
    url.searchParams.delete('view');
    url.searchParams.delete('browser');
    if (view === 'arc') {
      url.searchParams.set('view', 'arc');
    } else if (view === 'browser' && this.chosen_) {
      url.searchParams.set('browser', this.chosen_.id);
    }
    window.history.replaceState(null, '', url.pathname + url.search);
    const scroller = document.getElementById('scroller');
    if (scroller) {
      scroller.scrollTop = 0;
    } else {
      window.scrollTo(0, 0);
    }
  }

  // What the settings window's row for this page says.
  private updateDetail_() {
    if (!this.browsersLoaded_) {
      return;
    }
    const names: string[] = [];
    for (const profile of this.browsers_) {
      if (!names.includes(profile.browser)) {
        names.push(profile.browser);
      }
    }
    if (this.state_?.found) {
      names.push('Arc');
    }
    this.setSubpageDetail(
        names.length ? i18nF('arcImportPageFound', listNames(names)) :
                       i18n('arcImportPageNoneFound'));
  }

  // Other browsers: the list ------------------------------------------------

  private async refreshBrowsers_() {
    this.show('browsersLoading', true);
    this.show('browsersNone', false);
    ({profiles: this.browsers_} = await this.handler_.getBrowsers());
    this.browsersLoaded_ = true;
    this.renderBrowsers_();
    this.updateDetail_();
  }

  private isAvailable_(profile: BrowserImportProfile): boolean {
    const asSpace = !profile.imported;
    const asProfile = profile.asProfile && !profile.importedAsProfile;
    return !profile.running && (asSpace || asProfile);
  }

  private describeBrowser_(profile: BrowserImportProfile): string {
    const counts = [
      i18nPlural('arcImportPageBookmarkCount', profile.bookmarkCount),
      i18nPlural('arcImportPageOpenTabCount', profile.tabCount),
    ];
    if (profile.spaces.length > 1) {
      counts.push(i18nPlural('arcImportPageSpaceCount', profile.spaces.length));
    }
    return profile.note || counts.join(' \u00b7 ');
  }

  // A profile found: its row opens its choices; one that can't be imported
  // now says why.
  private renderBrowser_(profile: BrowserImportProfile): HTMLElement {
    const available = this.isAvailable_(profile);
    const row = element(available ? 'button' : 'div', 'row browser');
    row.dataset['id'] = profile.id;
    row.classList.toggle('unavailable', !available);
    const why = profile.running ?
        i18nF('arcImportPageBrowserOpen', profile.browser) :
        !available ? i18n('arcImportPageImportedAlready') :
                     '';
    const text = rowText(profileLabel(profile), this.describeBrowser_(profile));
    if (why) {
      text.append(element('span', 'row-detail warning', why));
    }
    row.append(browserIcon(profile.browser, profile.iconDataUrl), text);
    if (available) {
      row.append(chevron());
      row.addEventListener('click', () => this.openBrowser_(profile));
    }
    return row;
  }

  private renderBrowsers_() {
    this.show('browsersLoading', false);
    this.show('browsersNone', this.browsers_.length === 0);
    this.show(
        'browsersRunning', this.browsers_.some(profile => profile.running));
    this.$('browsers').replaceChildren(
        ...this.browsers_.map(profile => this.renderBrowser_(profile)));
  }

  // Other browsers: one profile's choices -----------------------------------

  private openBrowser_(profile: BrowserImportProfile, updatePath = true) {
    this.chosen_ = profile;
    this.show('browsersChooser', true);
    this.show('browsersImporting', false);
    this.show('browsersDone', false);
    const icon = browserIcon(profile.browser, profile.iconDataUrl, true);
    this.$('browserIcon').replaceChildren(icon);
    this.$('browserTitle').textContent = profileLabel(profile);
    this.$('browserDetail').textContent = this.describeBrowser_(profile);
    this.renderChoices_(profile);
    this.setView_('browser', updatePath);
  }

  // Space here, or a Compass profile of its own: the radio at the row's end.
  private renderMode_(
      value: string, icon: string, color: string, title: string,
      description: string, enabled: boolean, checked: boolean): HTMLElement {
    const row = element('label', 'row control-row');
    const input = element('input', 'radio mode-radio');
    input.type = 'radio';
    input.name = 'mode';
    input.value = value;
    input.checked = checked;
    input.disabled = !enabled;
    row.append(tile(icon, color), rowText(title, description), input);
    return row;
  }

  private renderOption_(
      kind: string, icon: string, color: string, title: string,
      detail: string): HTMLElement {
    const row = element('label', 'row control-row option');
    row.dataset['kind'] = kind;
    const input = element('input', 'switch option-check');
    input.type = 'checkbox';
    input.dataset['kind'] = kind;
    input.checked = true;
    input.setAttribute('aria-label', title);
    input.addEventListener('change', () => this.updateBrowsersButton_());
    row.append(tile(icon, color), rowText(title, detail), input);
    return row;
  }

  private renderChoices_(profile: BrowserImportProfile) {
    const asSpace = !profile.imported;
    const asProfile = profile.asProfile && !profile.importedAsProfile;
    const modes = this.$('modes');
    modes.replaceChildren();
    this.show('modeGroup', profile.asProfile);
    if (profile.asProfile) {
      let profileDescription = profile.importedAsProfile ?
          i18nF('arcImportPageImportedAsProfile', profile.importedAsProfile) :
          profile.asProfileBrings || i18n('arcImportPageKeptApart');
      if (asProfile && profile.newerVersion) {
        profileDescription += ' ' +
            i18nF('arcImportPageNewerVersion', profile.browser,
                  profile.newerVersion);
      }
      modes.append(
          this.renderMode_(
              'space', 'sidebar', 'purple', i18n('arcImportPageAsSpace'),
              profile.imported ?
                  i18n('arcImportPageImportedAlready') :
                  i18nF('arcImportPageNewSpaceCalled', profile.browser),
              asSpace, asSpace),
          this.renderMode_(
              'profile', 'profiles', 'blue', i18n('arcImportPageAsProfile'),
              profileDescription, asProfile, !asSpace && asProfile));
      for (const radio of modes.querySelectorAll<HTMLInputElement>(
               '.mode-radio')) {
        radio.addEventListener('change', () => this.updateMode_());
      }
    }

    const options = this.$('options');
    options.replaceChildren(
        this.renderOption_(
            'bookmarks', 'bookmark', 'blue', i18n('arcImportPageBookmarks'),
            i18nPlural(
                'arcImportPageTabsInPinnedOption', profile.bookmarkCount)),
        this.renderOption_(
            'tabs', 'sidebar', 'purple', i18n('arcImportPageOpenTabs'),
            i18nPlural('arcImportPageTabsInTodayOption', profile.tabCount)));
    if (profile.history) {
      options.append(this.renderOption_(
          'history', 'pulse', 'teal', i18n('arcImportPageHistory'),
          i18n('arcImportPageHistoryDetail')));
    }
    if (profile.passwords) {
      options.append(this.renderOption_(
          'passwords', 'passwords', 'gray', i18n('arcImportPagePasswords'),
          i18n('arcImportPagePasswordsDetail')));
    }
    this.show(
        'keychainNotice',
        isMac() && (profile.passwords || profile.asProfile));
    this.updateMode_();
  }

  private isNewProfile_(): boolean {
    return !!this.querySelector<HTMLInputElement>(
        '#modes .mode-radio[value="profile"]:checked');
  }

  // A new profile copies history and passwords: no choice to make.
  private updateMode_() {
    const newProfile = this.isNewProfile_();
    for (const kind of ['history', 'passwords']) {
      const row =
          this.querySelector<HTMLElement>(`#options .option[data-kind="${
              kind}"]`);
      if (row) {
        row.hidden = newProfile;
      }
    }
    this.updateBrowsersButton_();
  }

  private browserChoices_(): BrowserImportChoice[] {
    const profile = this.chosen_;
    if (!profile || !this.isAvailable_(profile)) {
      return [];
    }
    const option = (kind: string) => !!this.querySelector<HTMLInputElement>(
        `#options .option-check[data-kind="${kind}"]:checked`);
    const newProfile = this.isNewProfile_();
    const choice = {
      id: profile.id,
      bookmarks: option('bookmarks'),
      tabs: option('tabs'),
      // A new profile brings them with its copy.
      history: !newProfile && option('history'),
      passwords: !newProfile && option('passwords'),
      newProfile,
    };
    // A new profile is worth making even without its tabs.
    return choice.newProfile || choice.bookmarks || choice.tabs ||
            choice.history || choice.passwords ?
        [choice] :
        [];
  }

  private updateBrowsersButton_() {
    this.$<HTMLButtonElement>('importBrowsers').disabled =
        this.browserChoices_().length === 0;
  }

  private describeSummary_(summary: BrowserImportSummary): HTMLElement {
    const parts = [];
    if (summary.profile) {
      parts.push(i18nF('arcImportPageNewProfileNamed', summary.profile));
    }
    if (summary.spaces.length) {
      parts.push(i18nPlural(
          'arcImportPageSpacesNamed', summary.spaces.length,
          summary.spaces.join(', ')));
    }
    if (summary.pinned) {
      parts.push(i18nPlural('arcImportPageTabsInPinned', summary.pinned));
    }
    if (summary.today) {
      parts.push(i18nPlural('arcImportPageTabsInToday', summary.today));
    }
    if (summary.favorites) {
      parts.push(i18nPlural('arcImportPageFavoriteCount', summary.favorites));
    }
    if (summary.profile) {
      parts.push(summary.brought || i18n('arcImportPageProfileBrought'));
      if (summary.brought && summary.history > 0) {
        parts.push(i18nPlural('arcImportPageHistoryCount', summary.history));
      }
    } else {
      if (summary.history > 0) {
        parts.push(i18nPlural('arcImportPageHistoryCount', summary.history));
      } else if (summary.history < 0) {
        parts.push(i18n('arcImportPageHistoryBrought'));
      }
      if (summary.passwords > 0) {
        parts.push(i18nPlural('arcImportPagePasswordCount', summary.passwords));
      } else if (summary.passwords < 0) {
        parts.push(i18n('arcImportPagePasswordsBrought'));
      }
    }
    const problems = [...summary.problems];
    if (summary.passwordsSkipped) {
      problems.push(i18nPlural(
          'arcImportPagePasswordsSkipped', summary.passwordsSkipped));
    }
    if (summary.cookiesDropped) {
      problems.push(
          i18nPlural('arcImportPageCookiesDropped', summary.cookiesDropped));
    }
    const row = element('div', 'row summary');
    const text = rowText(
        summary.name,
        parts.length ? parts.join(' \u00b7 ') :
                       i18n('arcImportPageNothingImported'));
    for (const problem of problems) {
      text.append(element('span', 'row-detail warning', problem));
    }
    row.append(
        tile(parts.length ? 'check' : 'about', parts.length ? 'green' : 'gray'),
        text);
    return row;
  }

  private async importBrowsers_() {
    const choices = this.browserChoices_();
    if (!choices.length) {
      return;
    }
    this.show('browsersChooser', false);
    this.show('browsersImporting', true);
    const {summaries} = await this.handler_.importBrowsers(choices);
    this.show('browsersImporting', false);
    this.show('browsersDone', true);
    this.$('browsersSummary')
        .replaceChildren(
            ...summaries.map(summary => this.describeSummary_(summary)));
    this.show(
        'openBrowsers', summaries.some(summary => summary.spaces.length > 0));
  }

  // Arc ---------------------------------------------------------------------

  // New profiles to import.
  private chosenProfiles_(): string[] {
    return Array
        .from(this.querySelectorAll<HTMLInputElement>(
            '#profiles .profile-check:checked:not(:disabled)'))
        .map(input => input.value);
  }

  // Spaces to bring in, of new profiles or of profiles imported before.
  private chosenSpaces_(): string[] {
    return Array
        .from(this.querySelectorAll<HTMLInputElement>(
            '#profiles .space-check:checked:not(:disabled)'))
        .map(input => input.value);
  }

  private updateImportButton_() {
    this.$<HTMLButtonElement>('import').disabled = !this.state_ ||
        this.state_.arcRunning ||
        (this.chosenProfiles_().length === 0 &&
         this.chosenSpaces_().length === 0);
  }

  private renderSpace_(space: ArcImportSpace): HTMLLIElement {
    const item = document.createElement('li');
    item.classList.toggle('imported', space.imported);
    const label = document.createElement('label');
    const input = checkbox('space-check', space.id);
    input.checked = !space.imported;
    input.disabled = space.imported;
    input.addEventListener('change', () => this.updateImportButton_());
    const name = document.createElement('span');
    name.className = 'space-name';
    name.textContent = space.imported ?
        i18nF('arcImportPageImportedName', space.name) :
        space.name;
    const details = document.createElement('span');
    details.className = 'details';
    details.textContent = i18nPlural('arcImportPageTabCount', space.tabCount) +
        (space.archivedCount ?
             ` · ${
                 i18nPlural(
                     'arcImportPageArchivedCount', space.archivedCount)}` :
             '');
    label.append(input, name, details);
    item.append(label);
    return item;
  }

  private renderProfile_(profile: ArcImportProfile): HTMLLIElement {
    const item = document.createElement('li');
    item.className = 'profile';
    const newSpaces = profile.spaces.filter(space => !space.imported).length;
    // Imported before, nothing new: nothing to choose.
    item.classList.toggle('imported', profile.imported && !newSpaces);
    const label = document.createElement('label');
    const input = checkbox('profile-check', profile.dir);
    input.checked = true;
    input.disabled = profile.imported;
    const text = document.createElement('div');
    const name = document.createElement('div');
    name.className = 'name';
    name.textContent = profile.imported ?
        i18nF('arcImportPageImportedName', profile.name) :
        profile.name;
    const details = document.createElement('div');
    details.className = 'details';
    details.textContent = profile.imported && newSpaces ?
        i18nPlural('arcImportPageNewSpacesToAdd', newSpaces) :
        describe(profile);
    text.append(name, details);
    label.append(input, text);
    item.append(label);

    if (profile.spaces.length) {
      const spaces = document.createElement('ul');
      spaces.className = 'spaces';
      for (const space of profile.spaces) {
        spaces.append(this.renderSpace_(space));
      }
      item.append(spaces);
    }
    // A new profile's spaces come with it or not at all.
    if (!profile.imported) {
      input.addEventListener('change', () => {
        for (const space of item.querySelectorAll<HTMLInputElement>(
                 '.space-check')) {
          space.disabled = !input.checked;
        }
        this.updateImportButton_();
      });
    }
    return item;
  }

  private render_() {
    const state = this.state_;
    if (!state) {
      return;
    }
    this.show('loading', false);
    // Its row in the list.
    this.show('arcGroup', state.found);
    this.$('arcIcon').replaceChildren(browserIcon('Arc', state.iconDataUrl));
    const spaces = state.profiles.reduce(
        (count, profile) => count + profile.spaces.length, 0);
    this.$('arcDetail').textContent = state.arcRunning ?
        i18n('arcImportPageArcOpenShort') :
        `${
            i18nPlural(
                'arcImportPageProfileCount', state.profiles.length)} \u00b7 ${
            i18nPlural('arcImportPageSpaceCount', spaces)}`;
    this.updateDetail_();
    this.show('notFound', !state.found);
    this.show('chooser', state.found);
    this.show('arcRunning', state.arcRunning);
    this.show('arcKeychainNotice', state.found && isMac());
    this.$('profiles').replaceChildren(
        ...state.profiles.map(profile => this.renderProfile_(profile)));
    this.updateImportButton_();
  }

  private async refresh_() {
    ({state: this.state_} = await this.handler_.getState());
    this.render_();
  }

  private async import_() {
    const profiles = this.chosenProfiles_();
    const spaces = this.chosenSpaces_();
    if (!profiles.length && !spaces.length) {
      return;
    }
    this.show('chooser', false);
    this.show('importing', true);
    const {imported, updated, failed, undecryptable, easels, easelsMissing} =
        await this.handler_.import(profiles, spaces);
    this.show('importing', false);
    this.show('done', true);
    const sentences = [];
    if (imported.length) {
      sentences.push(
          i18nF('arcImportPageImportedProfiles', imported.join(', ')));
    }
    if (updated.length) {
      sentences.push(i18nF('arcImportPageAddedSpaces', updated.join(', ')));
    }
    if (easels) {
      sentences.push(i18nPlural('arcImportPageBroughtEasels', easels));
    }
    if (undecryptable) {
      sentences.push(i18nPlural('arcImportPageUndecryptable', undecryptable));
    }
    this.$('doneText').textContent = sentences.length ?
        sentences.join(' ') :
        i18n('arcImportPageNothingImported');
    this.show('failedText', failed.length > 0);
    this.$('failedText').textContent = failed.length ?
        i18nPlural(
            isMac() ? 'arcImportPageFailedMac' : 'arcImportPageFailed',
            failed.length) :
        '';
    this.show('open', imported.length > 0 || updated.length > 0);
    this.showMissingEasels_(easelsMissing);
  }

  // Easels Arc never downloaded to this Mac came over empty: Arc downloads
  // a board when it's opened.
  private showMissingEasels_(missing: number) {
    this.show('easelsText', missing > 0);
    this.show('importEasels', missing > 0);
    this.$('easelsText').textContent =
        missing ? i18nPlural('arcImportPageEaselsEmpty', missing) : '';
  }

  private async importEasels_() {
    const button = this.$<HTMLButtonElement>('importEasels');
    button.disabled = true;
    // Arc writes its cache while it runs.
    const {quit} = await this.handler_.quitArc();
    if (!quit) {
      button.disabled = false;
      this.$('easelsText').textContent =
          i18n('arcImportPageQuitThenImportEasels');
      return;
    }
    const {arcRunning, easels, easelsMissing} =
        await this.handler_.importEasels();
    button.disabled = false;
    if (arcRunning) {
      this.$('easelsText').textContent =
          i18n('arcImportPageQuitThenImportEasels');
      return;
    }
    this.showMissingEasels_(easelsMissing);
    if (easels) {
      const done = this.$('doneText');
      done.textContent = `${done.textContent ?? ''} ${
          i18nPlural('arcImportPageBroughtEasels', easels)}`;
    } else if (!easelsMissing) {
      this.$('easelsText').textContent = i18n('arcImportPageNothingNew');
      this.show('easelsText', true);
    }
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-arc-import-page': CompassArcImportPageElement;
  }
}

customElements.define('compass-arc-import-page', CompassArcImportPageElement);
