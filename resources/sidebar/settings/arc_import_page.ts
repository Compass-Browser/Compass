// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {ArcImportProfile, ArcImportState} from '../sidebar.mojom-webui.js';
import {ArcImportHandler} from '../sidebar.mojom-webui.js';

import {getTemplate} from './arc_import_page.html.js';
import {CompassSettingsPage} from './page_base.js';

// Importing from Arc: compass://settings/importArc, and alone on
// chrome://compass-import. See docs/design/arc-import.md.

function describe(profile: ArcImportProfile): string {
  const spaces = profile.spaces.length === 1 ?
      '1 space' :
      `${profile.spaces.length} spaces`;
  const names = profile.spaces.length ? ` (${profile.spaces.join(', ')})` : '';
  const tabs = profile.tabCount === 1 ? '1 tab' : `${profile.tabCount} tabs`;
  return `${spaces}${names} · ${tabs}`;
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

  private chosenDirs_(): string[] {
    return Array
        .from(this.querySelectorAll<HTMLInputElement>(
            '#profiles input:checked'))
        .map(input => input.value);
  }

  private updateImportButton_() {
    this.$<HTMLButtonElement>('import').disabled = !this.state_ ||
        this.state_.arcRunning || this.chosenDirs_().length === 0;
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
    const list = this.$('profiles');
    list.replaceChildren();
    for (const profile of state.profiles) {
      const item = document.createElement('li');
      item.classList.toggle('imported', profile.imported);
      const label = document.createElement('label');
      const checkbox = document.createElement('input');
      checkbox.type = 'checkbox';
      checkbox.value = profile.dir;
      checkbox.checked = !profile.imported;
      checkbox.disabled = profile.imported;
      checkbox.addEventListener('change', () => this.updateImportButton_());
      const text = document.createElement('div');
      const name = document.createElement('div');
      name.className = 'name';
      name.textContent =
          profile.imported ? `${profile.name} (imported)` : profile.name;
      const details = document.createElement('div');
      details.className = 'details';
      details.textContent = describe(profile);
      text.append(name, details);
      label.append(checkbox, text);
      item.append(label);
      list.append(item);
    }
    this.updateImportButton_();
  }

  private async refresh_() {
    ({state: this.state_} = await this.handler_.getState());
    this.render_();
  }

  private async import_() {
    const dirs = this.chosenDirs_();
    if (!dirs.length) {
      return;
    }
    this.show('chooser', false);
    this.show('importing', true);
    const {imported, failed} = await this.handler_.import(dirs);
    this.show('importing', false);
    this.show('done', true);
    this.$('doneText').textContent = imported.length ?
        `Imported ${imported.join(', ')}.` :
        'Nothing was imported.';
    this.show('failedText', failed.length > 0);
    this.$('failedText').textContent = failed.length ?
        `Couldn't import ${
            failed.length === 1 ? 'one profile' :
                                  `${failed.length} profiles`}` +
            '. Check that Arc is closed and that Compass was allowed to use ' +
            '"Arc Safe Storage", then try again.' :
        '';
    this.show('open', imported.length > 0);
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-arc-import-page': CompassArcImportPageElement;
  }
}

customElements.define('compass-arc-import-page', CompassArcImportPageElement);
