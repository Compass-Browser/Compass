// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// chrome://easel/: the profile's Easels, newest first, to open, rename,
// delete (after asking) and make (docs/design/easels.md, "In the sidebar").

import {EaselBrowserProxy} from './browser_proxy.js';
import {button, el, toast} from './dom.js';
import type {EaselSummary} from './easel.mojom-webui.js';
import {i18n, i18nF, i18nPlural} from './i18n.js';

const NEW_TITLE = i18n('easelUntitled');

export class EaselList {
  private readonly root_: HTMLElement;
  private readonly proxy_ = EaselBrowserProxy.getInstance();
  private list_: HTMLElement|null = null;
  private renaming_ = '';
  private refreshTimer_ = 0;

  constructor(root: HTMLElement) {
    this.root_ = root;
    this.proxy_.callbackRouter.onEaselChanged.addListener(() => {
      clearTimeout(this.refreshTimer_);
      this.refreshTimer_ = setTimeout(() => void this.refresh_(), 100);
    });
  }

  async start() {
    document.title = i18n('easelList');
    const create = button(i18n('easelNewEasel'), 'primary', () => void this.create_());
    this.list_ = el('ul', {className: 'easel-list'});
    this.root_.replaceChildren(el('main', {className: 'list-page'}, [
      el('header', {className: 'list-header'}, [
        el('h1', {text: i18n('easelList')}),
        create,
      ]),
      this.list_,
    ]));
    await this.refresh_();
  }

  private async refresh_() {
    if (this.renaming_) {
      // Not under the user's typing; the next change refreshes.
      return;
    }
    const {available, easels} = await this.proxy_.handler.getEasels();
    const list = this.list_!;
    if (!available) {
      list.replaceChildren(el('li', {
        className: 'empty',
        text: i18n('easelUnavailable'),
      }));
      return;
    }
    if (easels.length === 0) {
      list.replaceChildren(el('li', {
        className: 'empty',
        text: i18n('easelNoEasels'),
      }));
      return;
    }
    list.replaceChildren(...easels.map(e => this.row_(e)));
  }

  private row_(easel: EaselSummary): HTMLElement {
    const open = el('a', {
      className: 'easel-open',
      attrs: {'href': `/${easel.uuid}`},
    }, [
      el('span', {className: 'easel-title', text: easel.title || NEW_TITLE}),
      el('span', {className: 'easel-meta', text: describe(easel)}),
    ]);
    const row = el('li', {className: 'easel-row'}, [
      open,
      button(i18n('easelRename'), 'text-button', () => this.rename_(row, easel)),
      button(i18n('easelDelete'), 'text-button danger', () => this.delete_(easel)),
    ]);
    row.dataset['palette'] = easel.palette || 'paper';
    return row;
  }

  private async create_() {
    const {uuid, error} = await this.proxy_.handler.createEasel(NEW_TITLE);
    if (error) {
      toast(error);
      return;
    }
    location.assign(`/${uuid}`);
  }

  private rename_(row: HTMLElement, easel: EaselSummary) {
    const title = row.querySelector('.easel-title');
    if (!title) {
      return;
    }
    const input = el('input', {
      className: 'rename',
      attrs: {
        'type': 'text',
        'aria-label': i18n('easelTitleLabel'),
        'maxlength': '200',
        'value': easel.title,
      },
    });
    this.renaming_ = easel.uuid;
    let done = false;
    const finish = (save: boolean) => {
      if (done) {
        return;
      }
      done = true;
      this.renaming_ = '';
      if (save && input.value.trim() !== easel.title) {
        this.proxy_.handler.renameEasel(easel.uuid, input.value.trim());
      }
      void this.refresh_();
    };
    input.addEventListener('keydown', (e: KeyboardEvent) => {
      if (e.key === 'Enter') {
        finish(true);
      } else if (e.key === 'Escape') {
        finish(false);
      }
    });
    input.addEventListener('blur', () => finish(true));
    title.replaceWith(input);
    input.focus();
    input.select();
  }

  private delete_(easel: EaselSummary) {
    const dialog = el('dialog', {className: 'confirm'});
    const name = easel.title || NEW_TITLE;
    dialog.append(
        el('p', {text: i18nF('easelDeleteQuestion', name)}),
        el('p', {
          className: 'detail',
          text: i18n('easelDeleteDetail'),
        }),
        el('div', {className: 'buttons'}, [
          button(i18n('easelCancel'), 'text-button', () => dialog.close()),
          button(i18n('easelDelete'), 'primary danger', () => {
            dialog.close();
            this.proxy_.handler.deleteEasel(easel.uuid);
            void this.refresh_();
          }),
        ]));
    dialog.addEventListener('close', () => dialog.remove());
    document.body.appendChild(dialog);
    dialog.showModal();
  }
}

function describe(easel: EaselSummary): string {
  const count = i18nPlural('easelItemCount', easel.objectCount);
  const time = easel.updateTimeMs || easel.createTimeMs;
  if (!time) {
    return count;
  }
  const date = new Date(time);
  const sameYear = date.getFullYear() === new Date().getFullYear();
  const when = date.toLocaleDateString(undefined, {
    month: 'short',
    day: 'numeric',
    year: sameYear ? undefined : 'numeric',
  });
  return `${count} · ${when}`;
}
