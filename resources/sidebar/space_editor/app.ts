// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// chrome://compass-space-editor/?id=<space>: a space's editor, in a bubble
// beside the sidebar (docs/design/spaces.md, "UI"). Its name, theme (the
// theme editor, in the bubble's place), icon (a symbol, or an emoji from the
// picker, also in its place), when Today tabs are archived, its profile,
// whether it syncs, Share and Delete. Edits go to the browser as they're
// made; the space comes back from it (SpaceChanged()).

import '../emoji_picker.js';
import '../theme_editor.js';

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';
import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {applyBubbleLook} from '../bubble_look.js';
import {colorToCss} from '../colors.js';
import {i18n} from '../i18n.js';
import type {ProfileOption, Space} from '../sidebar.mojom-webui.js';
import type {SpaceSymbol} from '../space_icons.js';
import {SPACE_SYMBOL_VIEW_BOX, SPACE_SYMBOLS} from '../space_icons.js';
import type {ArchiveDelay} from '../space_options.js';
import {ARCHIVE_DELAYS} from '../space_options.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import {SpaceEditorBrowserProxy} from './browser_proxy.js';

// What the bubble shows: the editor, or the theme editor or the emoji
// picker in its place.
export type View = 'main'|'theme'|'emoji';

// A question the editor asks in place of a dialog (a bubble has none).
export type Confirming = 'delete'|'unsync'|null;

export class CompassSpaceEditorAppElement extends CrLitElement {
  static get is() {
    return 'compass-space-editor-app';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      space_: {type: Object},
      profiles_: {type: Array},
      canDelete_: {type: Boolean},
      loaded_: {type: Boolean},
      view_: {type: String},
      confirming_: {type: String},
    };
  }

  // Null once loaded: the space is gone.
  protected accessor space_: Space|null = null;
  protected accessor profiles_: ProfileOption[] = [];
  protected accessor canDelete_: boolean = false;
  protected accessor loaded_: boolean = false;
  protected accessor view_: View = 'main';
  protected accessor confirming_: Confirming = null;
  protected readonly spaceSymbols_: SpaceSymbol[] = SPACE_SYMBOLS;
  protected readonly archiveDelays_: ArchiveDelay[] = ARCHIVE_DELAYS;

  protected readonly proxy_ = SpaceEditorBrowserProxy.getInstance();
  private shown_ = false;

  override connectedCallback() {
    super.connectedCallback();
    ColorChangeUpdater.forDocument().start();
    this.proxy_.callbackRouter.spaceChanged.addListener(
        () => this.loadSpace_());
    window.matchMedia('(prefers-color-scheme: dark)')
        .addEventListener('change', () => this.applyLook_());
    document.addEventListener('keydown', this.onKeydown_);
    this.start_();
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    document.removeEventListener('keydown', this.onKeydown_);
  }

  private async start_() {
    await this.applyLook_();
    await this.loadSpace_();
    await this.updateComplete;
    // The name is what's most often changed.
    this.shadowRoot.querySelector<HTMLInputElement>('#name')?.select();
    if (!this.shown_) {
      this.shown_ = true;
      this.proxy_.handler.showUI();
    }
  }

  private async applyLook_() {
    const {look} = await this.proxy_.handler.getLook();
    applyBubbleLook(look);
  }

  private async loadSpace_() {
    const {space, profiles, canDelete} = await this.proxy_.handler.getSpace();
    this.space_ = space;
    this.profiles_ = profiles;
    this.canDelete_ = canDelete;
    this.loaded_ = true;
    if (!space) {
      // Deleted, here or elsewhere.
      this.proxy_.handler.close();
    }
  }

  // Esc goes back from the theme editor or the picker (whose own Esc clears
  // its search first), and closes the bubble from the editor.
  private onKeydown_ = (e: KeyboardEvent) => {
    if (e.key !== 'Escape' || e.defaultPrevented) {
      return;
    }
    e.preventDefault();
    if (this.confirming_) {
      this.confirming_ = null;
    } else if (this.view_ !== 'main') {
      this.view_ = 'main';
    } else {
      this.proxy_.handler.close();
    }
  };

  protected onCloseClick_() {
    this.proxy_.handler.close();
  }

  // The name -----------------------------------------------------------------

  protected onNameChange_(e: Event) {
    this.rename_(e.target as HTMLInputElement);
  }

  protected onNameKeydown_(e: KeyboardEvent) {
    if (e.key === 'Enter') {
      this.rename_(e.target as HTMLInputElement);
      this.proxy_.handler.close();
    }
  }

  private rename_(input: HTMLInputElement) {
    const name = input.value.trim();
    if (this.space_ && name && name !== this.space_.name) {
      this.proxy_.handler.renameSpace(this.space_.id, name);
    }
  }

  // The theme ----------------------------------------------------------------

  // The chip's colors: the space's gradient's three stops, all its color
  // for a plain one, as the sidebar's tint samples them (sidebar_tint.cc).
  protected getThemeChipStyle_(): string {
    const space = this.space_!;
    const colors = [space.color, ...space.gradient];
    const stop = (at: number) => {
      const index = Math.round(at * (colors.length - 1));
      return colorToCss(colors[index] ?? null);
    };
    return `--space-color: ${stop(0)}; --space-color-2: ${stop(0.5)}; ` +
        `--space-color-3: ${stop(1)}`;
  }

  protected onThemeClick_() {
    this.confirming_ = null;
    this.view_ = 'theme';
  }

  protected onBackClick_() {
    this.view_ = 'main';
  }

  // The icon -----------------------------------------------------------------

  protected getInitial_(): string {
    return Array.from(this.space_!.name.trim())[0]?.toUpperCase() ?? '•';
  }

  protected getSymbolViewBox_(): string {
    return SPACE_SYMBOL_VIEW_BOX;
  }

  // |symbol| '' stands for no icon.
  protected isIcon_(symbol: string): boolean {
    const icon = this.space_?.icon;
    return symbol ? icon?.symbol === symbol : !icon;
  }

  protected getEmoji_(): string {
    return this.space_?.icon?.emoji ?? '';
  }

  protected onSymbolClick_(e: Event) {
    const name = (e.currentTarget as HTMLElement).dataset['symbol'];
    if (this.space_) {
      this.proxy_.handler.setSpaceIcon(
          this.space_.id, name ? {symbol: name} : null);
    }
  }

  protected onEmojiClick_() {
    this.confirming_ = null;
    this.view_ = 'emoji';
  }

  // Sets the emoji and goes back to the editor.
  protected onEmojiPick_(e: CustomEvent<{emoji: string}>) {
    if (this.space_) {
      this.proxy_.handler.setSpaceIcon(this.space_.id, {emoji: e.detail.emoji});
    }
    this.view_ = 'main';
  }

  // Archiving and the profile ------------------------------------------------

  protected onArchiveDelayChange_(e: Event) {
    if (this.space_) {
      this.proxy_.handler.setArchiveAfter(
          this.space_.id, Number((e.target as HTMLSelectElement).value));
    }
  }

  protected hasProfiles_(): boolean {
    return this.profiles_.length > 0;
  }

  // Assigns the space to the chosen profile, or a new one (''). The bubble
  // closes: the space moves out of the window's profile.
  protected onProfileChange_(e: Event) {
    const id = (e.target as HTMLSelectElement).value;
    if (!this.space_ || id === this.space_.profileId) {
      return;
    }
    this.proxy_.handler.setSpaceProfile(this.space_.id, id || null);
    this.proxy_.handler.close();
  }

  // Sync ---------------------------------------------------------------------

  // "Sync to your other devices": shown while the profile's spaces sync, or
  // for a space already kept on this device (docs/design/sync.md, "Spaces
  // that don't sync").
  protected canChangeSync_(): boolean {
    return !!this.space_ && (this.space_.syncOn || !this.space_.synced);
  }

  // Turning it off removes the space from the other devices: asked first.
  protected onSyncClick_() {
    const space = this.space_;
    if (!space) {
      return;
    }
    if (space.synced) {
      this.confirming_ = 'unsync';
      return;
    }
    this.proxy_.handler.setSpaceSynced(space.id, true);
  }

  protected onUnsyncConfirmClick_() {
    this.confirming_ = null;
    if (this.space_) {
      this.proxy_.handler.setSpaceSynced(this.space_.id, false);
    }
  }

  // Sharing and deleting -----------------------------------------------------

  protected canShare_(): boolean {
    return !!this.space_ && this.space_.ownProfile && this.space_.canShare;
  }

  protected getShareLabel_(): string {
    return this.space_?.shared ? i18n('sidebarSharingMenu') :
                                 i18n('sidebarShareMenu');
  }

  // The share bubble takes this one's place.
  protected onShareClick_() {
    if (this.space_) {
      this.proxy_.handler.shareSpace(this.space_.id);
    }
  }

  protected onDeleteClick_() {
    this.confirming_ = 'delete';
  }

  protected onDeleteConfirmClick_() {
    if (this.space_) {
      this.proxy_.handler.deleteSpace(this.space_.id);
    }
    this.proxy_.handler.close();
  }

  protected onConfirmCancelClick_() {
    this.confirming_ = null;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-space-editor-app': CompassSpaceEditorAppElement;
  }
}

customElements.define(
    CompassSpaceEditorAppElement.is, CompassSpaceEditorAppElement);
