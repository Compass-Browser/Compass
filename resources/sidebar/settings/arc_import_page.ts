// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {ArcImportProfile, ArcImportSpace, ArcImportState} from '../sidebar.mojom-webui.js';
import {ArcImportHandler} from '../sidebar.mojom-webui.js';

import {getTemplate} from './arc_import_page.html.js';
import {CompassSettingsPage} from './page_base.js';

// Importing from Arc: compass://settings/importArc, and alone on
// chrome://compass-import. See docs/design/arc-import.md.

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

  protected get pageTitle() {
    return 'Import from Arc';
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

    this.refresh_();
  }

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
    details.textContent = plural(space.tabCount, 'tab', 'tabs');
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
    this.show('notFound', !state.found);
    this.show('chooser', state.found);
    this.show('arcRunning', state.arcRunning);
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
    const {imported, updated, failed} =
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
    this.$('doneText').textContent =
        sentences.length ? sentences.join(' ') : 'Nothing was imported.';
    this.show('failedText', failed.length > 0);
    this.$('failedText').textContent = failed.length ?
        `Couldn't import ${
            failed.length === 1 ? 'one profile' :
                                  `${failed.length} profiles`}` +
            '. Check that Arc is closed and that Compass was allowed to use ' +
            '"Arc Safe Storage", then try again.' :
        '';
    this.show('open', imported.length > 0 || updated.length > 0);
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-arc-import-page': CompassArcImportPageElement;
  }
}

customElements.define('compass-arc-import-page', CompassArcImportPageElement);
