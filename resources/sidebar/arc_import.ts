// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';

import type {ArcImportProfile, ArcImportState} from './sidebar.mojom-webui.js';
import {ArcImportHandler} from './sidebar.mojom-webui.js';

// chrome://compass-import: importing from Arc. See
// docs/design/arc-import.md.
const handler = ArcImportHandler.getRemote();

ColorChangeUpdater.forDocument().start();

function $<T extends HTMLElement>(id: string): T {
  return document.getElementById(id) as T;
}

function show(id: string, visible: boolean) {
  $(id).hidden = !visible;
}

let state: ArcImportState|null = null;

function chosenDirs(): string[] {
  return Array
      .from(document.querySelectorAll<HTMLInputElement>(
          '#profiles input:checked'))
      .map(input => input.value);
}

function updateImportButton() {
  $<HTMLButtonElement>('import').disabled =
      !state || state.arcRunning || chosenDirs().length === 0;
}

function describe(profile: ArcImportProfile): string {
  const spaces = profile.spaces.length === 1 ?
      '1 space' :
      `${profile.spaces.length} spaces`;
  const names = profile.spaces.length ? ` (${profile.spaces.join(', ')})` : '';
  const tabs = profile.tabCount === 1 ? '1 tab' : `${profile.tabCount} tabs`;
  return `${spaces}${names} · ${tabs}`;
}

function render() {
  if (!state) {
    return;
  }
  show('loading', false);
  show('notFound', !state.found);
  show('chooser', state.found);
  show('arcRunning', state.arcRunning);
  const list = $('profiles');
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
    checkbox.addEventListener('change', updateImportButton);
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
  updateImportButton();
}

async function refresh() {
  ({state} = await handler.getState());
  render();
}

$('quitArc').addEventListener('click', async () => {
  $<HTMLButtonElement>('quitArc').disabled = true;
  await handler.quitArc();
  $<HTMLButtonElement>('quitArc').disabled = false;
  await refresh();
});

$('import').addEventListener('click', async () => {
  const dirs = chosenDirs();
  if (!dirs.length) {
    return;
  }
  show('chooser', false);
  show('importing', true);
  const {imported, failed} = await handler.import(dirs);
  show('importing', false);
  show('done', true);
  $('doneText').textContent = imported.length ?
      `Imported ${imported.join(', ')}.` :
      'Nothing was imported.';
  show('failedText', failed.length > 0);
  $('failedText').textContent = failed.length ?
      `Couldn't import ${failed.length === 1 ? 'one profile' :
                                               `${failed.length} profiles`}` +
          '. Check that Arc is closed and that Compass was allowed to use ' +
          '"Arc Safe Storage", then try again.' :
      '';
  show('open', imported.length > 0);
});

$('open').addEventListener('click', () => handler.openImported());

refresh();
