// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {ArcImportProfile, ArcImportSpace, ArcImportState, BrowserImportChoice, BrowserImportProfile, BrowserImportSummary} from '../sidebar.mojom-webui.js';
import {ArcImportHandler} from '../sidebar.mojom-webui.js';

import {getTemplate} from './arc_import_page.html.js';
import {CompassSettingsPage} from './page_base.js';

// Importing from another browser: compass://settings/importArc, and alone
// on chrome://compass-import. Other browsers' profiles first (each becomes
// a space, or a Compass profile of its own for Chromium-based ones), then
// Arc's (each becomes a profile), when Arc is there. See
// docs/design/browser-import.md and docs/design/arc-import.md.

// macOS asks before Compass reads another browser's Keychain key.
function isMac(): boolean {
  return navigator.userAgent.includes('Mac');
}

function plural(count: number, one: string, many: string): string {
  return count === 1 ? `1 ${one}` : `${count} ${many}`;
}

function describe(profile: ArcImportProfile): string {
  return `${plural(profile.spaces.length, 'space', 'spaces')} · ${
      plural(profile.tabCount, 'tab', 'tabs')}`;
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

  protected get pageTitle() {
    return 'Import from Another Browser';
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
      this.show('browsersDone', false);
      this.refreshBrowsers_();
    });

    this.refreshBrowsers_();
    this.refresh_();
  }

  // Other browsers ---------------------------------------------------------

  private async refreshBrowsers_() {
    this.show('browsersChooser', false);
    this.show('browsersLoading', true);
    ({profiles: this.browsers_} = await this.handler_.getBrowsers());
    this.renderBrowsers_();
  }

  private browserChoices_(): BrowserImportChoice[] {
    const choices: BrowserImportChoice[] = [];
    for (const row of this.querySelectorAll<HTMLElement>(
             '#browsers li.browser')) {
      const check =
          row.querySelector<HTMLInputElement>('.browser-check')!;
      if (!check.checked || check.disabled) {
        continue;
      }
      const option = (kind: string) =>
          !!row.querySelector<HTMLInputElement>(
                   `.option-check[data-kind="${kind}"]:checked`);
      const newProfile = !!row.querySelector<HTMLInputElement>(
          '.mode-radio[value="profile"]:checked');
      const choice = {
        id: check.value,
        bookmarks: option('bookmarks'),
        tabs: option('tabs'),
        // A new profile brings them with its copy.
        history: !newProfile && option('history'),
        passwords: !newProfile && option('passwords'),
        newProfile,
      };
      // A new profile is worth making even without its tabs.
      if (choice.newProfile || choice.bookmarks || choice.tabs ||
          choice.history || choice.passwords) {
        choices.push(choice);
      }
    }
    return choices;
  }

  private updateBrowsersButton_() {
    this.$<HTMLButtonElement>('importBrowsers').disabled =
        this.browserChoices_().length === 0;
  }

  private renderOption_(kind: string, text: string, enabled: boolean):
      HTMLLIElement {
    const item = document.createElement('li');
    const label = document.createElement('label');
    const input = checkbox('option-check', kind);
    input.dataset['kind'] = kind;
    input.checked = enabled;
    input.disabled = !enabled;
    input.addEventListener('change', () => this.updateBrowsersButton_());
    const name = document.createElement('span');
    name.textContent = text;
    label.append(input, name);
    item.append(label);
    return item;
  }

  // Space here, or a Compass profile of its own: a radio and what it means.
  private renderMode_(
      profile: BrowserImportProfile, value: string, title: string,
      description: string, enabled: boolean, checked: boolean): HTMLLIElement {
    const item = document.createElement('li');
    const label = document.createElement('label');
    const input = document.createElement('input');
    input.type = 'radio';
    input.className = 'mode-radio';
    input.name = `mode-${profile.id}`;
    input.value = value;
    input.checked = checked;
    input.disabled = !enabled;
    const text = document.createElement('div');
    const name = document.createElement('div');
    name.textContent = title;
    const details = document.createElement('div');
    details.className = 'details';
    details.textContent = description;
    text.append(name, details);
    label.append(input, text);
    item.append(label);
    return item;
  }

  private renderBrowser_(profile: BrowserImportProfile): HTMLLIElement {
    const item = document.createElement('li');
    item.className = 'browser';
    const asSpace = !profile.imported;
    const asProfile = profile.asProfile && !profile.importedAsProfile;
    const available = !profile.running && (asSpace || asProfile);
    item.classList.toggle('unavailable', !available);
    const label = document.createElement('label');
    const input = checkbox('browser-check', profile.id);
    input.checked = available;
    input.disabled = !available;
    const text = document.createElement('div');
    const name = document.createElement('div');
    name.className = 'name';
    name.textContent = profile.name ?
        `${profile.browser} \u2013 ${profile.name}` :
        profile.browser;
    const details = document.createElement('div');
    details.className = 'details';
    const counts = [
      plural(profile.bookmarkCount, 'bookmark', 'bookmarks'),
      plural(profile.tabCount, 'open tab', 'open tabs'),
    ];
    if (profile.spaces.length > 1) {
      counts.push(plural(profile.spaces.length, 'space', 'spaces'));
    }
    details.textContent = profile.running ?
        `${profile.browser} is open. Quit it to import from it.` :
        !available ? 'Imported already.' :
        profile.note     ? profile.note :
                           counts.join(' \u00b7 ');
    text.append(name, details);
    label.append(input, text);
    item.append(label);

    let modes: HTMLUListElement|null = null;
    if (profile.asProfile) {
      modes = document.createElement('ul');
      modes.className = 'modes';
      let profileDescription = profile.importedAsProfile ?
          `Imported already, as the profile ${profile.importedAsProfile}.` :
          'A profile of its own, with its cookies, history, passwords, ' +
              'extensions and settings: you stay signed in to sites.';
      if (asProfile && profile.newerVersion) {
        profileDescription += ` ${profile.browser} ${
            profile.newerVersion} is newer than Compass, so some of it ` +
            'may not come along.';
      }
      modes.append(
          this.renderMode_(
              profile, 'space', 'Add as a space here',
              profile.imported ? 'Imported already.' :
                                 'Its bookmarks and tabs, in this profile.',
              available && asSpace, asSpace),
          this.renderMode_(
              profile, 'profile', 'Add as a new profile', profileDescription,
              available && asProfile, !asSpace && asProfile));
      item.append(modes);
    }

    const options = document.createElement('ul');
    options.className = 'options';
    options.append(
        this.renderOption_('bookmarks', 'Bookmarks to Pinned', available),
        this.renderOption_('tabs', 'Open tabs to Today', available));
    if (profile.history) {
      options.append(this.renderOption_('history', 'History', available));
    }
    if (profile.passwords) {
      options.append(
          this.renderOption_('passwords', 'Passwords', available));
    }
    item.append(options);

    // A new profile copies history and passwords: no choice to make.
    const updateMode = () => {
      const newProfile = !!modes?.querySelector<HTMLInputElement>(
          '.mode-radio[value="profile"]:checked');
      for (const kind of ['history', 'passwords']) {
        const option = options.querySelector<HTMLInputElement>(
            `.option-check[data-kind="${kind}"]`);
        if (option) {
          option.closest('li')!.hidden = newProfile;
        }
      }
      this.updateBrowsersButton_();
    };
    for (const radio of modes?.querySelectorAll<HTMLInputElement>(
             '.mode-radio') ?? []) {
      radio.addEventListener('change', updateMode);
    }
    updateMode();

    input.addEventListener('change', () => {
      for (const option of options.querySelectorAll<HTMLInputElement>(
               '.option-check')) {
        option.disabled = !input.checked;
      }
      for (const radio of modes?.querySelectorAll<HTMLInputElement>(
               '.mode-radio') ?? []) {
        radio.disabled = !input.checked ||
            (radio.value === 'space' ? !asSpace : !asProfile);
      }
      this.updateBrowsersButton_();
    });
    return item;
  }

  private renderBrowsers_() {
    this.show('browsersLoading', false);
    this.show('browsersNone', this.browsers_.length === 0);
    this.show('browsersChooser', this.browsers_.length > 0);
    this.show(
        'browsersRunning', this.browsers_.some(profile => profile.running));
    this.show(
        'keychainNotice',
        isMac() &&
            this.browsers_.some(
                profile => profile.passwords || profile.asProfile));
    this.$('browsers').replaceChildren(
        ...this.browsers_.map(profile => this.renderBrowser_(profile)));
    this.updateBrowsersButton_();
  }

  private describeSummary_(summary: BrowserImportSummary): HTMLLIElement {
    const item = document.createElement('li');
    const name = document.createElement('div');
    name.className = 'name';
    name.textContent = summary.name;
    const parts = [];
    if (summary.profile) {
      parts.push(`New profile ${summary.profile}`);
    }
    if (summary.spaces.length) {
      parts.push(`${summary.spaces.length === 1 ? 'Space' : 'Spaces'} ${
          summary.spaces.join(', ')}`);
    }
    if (summary.pinned) {
      parts.push(`${plural(summary.pinned, 'tab', 'tabs')} in Pinned`);
    }
    if (summary.today) {
      parts.push(`${plural(summary.today, 'tab', 'tabs')} in Today`);
    }
    if (summary.favorites) {
      parts.push(plural(summary.favorites, 'favorite', 'favorites'));
    }
    if (summary.profile) {
      parts.push('cookies, history, passwords, extensions and settings');
    } else {
      if (summary.history > 0) {
        parts.push(plural(summary.history, 'history page', 'history pages'));
      } else if (summary.history < 0) {
        parts.push('history');
      }
      if (summary.passwords > 0) {
        parts.push(plural(summary.passwords, 'password', 'passwords'));
      } else if (summary.passwords < 0) {
        parts.push('passwords');
      }
    }
    const details = document.createElement('div');
    details.className = 'details';
    details.textContent =
        parts.length ? parts.join(' \u00b7 ') : 'Nothing was imported.';
    item.append(name, details);
    const problems = [...summary.problems];
    if (summary.passwordsSkipped) {
      problems.push(`${
          plural(summary.passwordsSkipped, 'password', 'passwords')} ` +
          'couldn\'t be decrypted and stayed behind.');
    }
    if (summary.cookiesDropped) {
      problems.push(`${
          plural(summary.cookiesDropped, 'cookie', 'cookies')} couldn't ` +
          'come along: you may have to sign in again on some sites.');
    }
    for (const problem of problems) {
      const line = document.createElement('div');
      line.className = 'details';
      line.textContent = problem;
      item.append(line);
    }
    return item;
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
    name.textContent =
        space.imported ? `${space.name} (imported)` : space.name;
    const details = document.createElement('span');
    details.className = 'details';
    details.textContent = plural(space.tabCount, 'tab', 'tabs') +
        (space.archivedCount ? ` · ${space.archivedCount} archived` : '');
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
    name.textContent =
        profile.imported ? `${profile.name} (imported)` : profile.name;
    const details = document.createElement('div');
    details.className = 'details';
    details.textContent = profile.imported && newSpaces ?
        `${plural(newSpaces, 'new space', 'new spaces')} to add. Its ` +
            'cookies, history and passwords stay as first imported.' :
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
    this.show('arcSection', state.found);
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
      sentences.push(`Imported ${imported.join(', ')}.`);
    }
    if (updated.length) {
      sentences.push(`Added new spaces to ${updated.join(', ')}.`);
    }
    if (easels) {
      sentences.push(`Brought over ${plural(easels, 'Easel', 'Easels')}.`);
    }
    if (undecryptable) {
      sentences.push(`${
          plural(undecryptable, 'cookie or password',
                 'cookies and passwords')} couldn't be decrypted and ` +
          'stayed behind: you may have to sign in again on some sites.');
    }
    this.$('doneText').textContent =
        sentences.length ? sentences.join(' ') : 'Nothing was imported.';
    this.show('failedText', failed.length > 0);
    this.$('failedText').textContent = failed.length ?
        `Couldn't import ${
            failed.length === 1 ? 'one profile' :
                                  `${failed.length} profiles`}` +
            (isMac() ? '. Check that Arc is closed and that Compass was ' +
                     'allowed to use "Arc Safe Storage", then try again.' :
                       '. Check that Arc is closed, then try again.') :
        '';
    this.show('open', imported.length > 0 || updated.length > 0);
    this.showMissingEasels_(easelsMissing);
  }

  // Easels Arc never downloaded to this Mac came over empty: Arc downloads
  // a board when it's opened.
  private showMissingEasels_(missing: number) {
    this.show('easelsText', missing > 0);
    this.show('importEasels', missing > 0);
    this.$('easelsText').textContent = missing ?
        `${plural(missing, 'Easel', 'Easels')} came over empty: Arc ` +
            'hasn\'t downloaded ' + (missing === 1 ? 'it' : 'them') +
            ' to this Mac. Open ' + (missing === 1 ? 'it' : 'them') +
            ' in Arc, then import again.' :
        '';
  }

  private async importEasels_() {
    const button = this.$<HTMLButtonElement>('importEasels');
    button.disabled = true;
    // Arc writes its cache while it runs.
    const {quit} = await this.handler_.quitArc();
    if (!quit) {
      button.disabled = false;
      this.$('easelsText').textContent =
          'Quit Arc, then choose Import Easels Again.';
      return;
    }
    const {arcRunning, easels, easelsMissing} =
        await this.handler_.importEasels();
    button.disabled = false;
    if (arcRunning) {
      this.$('easelsText').textContent =
          'Quit Arc, then choose Import Easels Again.';
      return;
    }
    this.showMissingEasels_(easelsMissing);
    if (easels) {
      const done = this.$('doneText');
      done.textContent = `${done.textContent ?? ''} Brought over ${
          plural(easels, 'Easel', 'Easels')}.`;
    } else if (!easelsMissing) {
      this.$('easelsText').textContent = 'Nothing new to bring over.';
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
