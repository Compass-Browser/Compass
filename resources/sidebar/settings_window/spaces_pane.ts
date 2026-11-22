// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {colorToCss} from '../colors.js';
import type {ProfileOption, Space} from '../sidebar.mojom-webui.js';
import type {SpaceSymbol} from '../space_icons.js';
import {findSpaceSymbol, SPACE_SYMBOL_VIEW_BOX, SPACE_SYMBOLS} from '../space_icons.js';
import type {ArchiveDelay} from '../space_options.js';
import {ARCHIVE_DELAYS, firstGrapheme} from '../space_options.js';
import '../theme_editor.js';
import type {ThemeEditorHandler} from '../theme_editor.js';

import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import {getHtml} from './spaces_pane.html.js';

// A profile's spaces, as the pane groups them.
export interface SpaceGroup {
  profileId: string;
  profileName: string;
  spaces: Space[];
}

// Spaces (docs/design/settings-window.md): every profile's spaces, grouped
// by profile, with what the sidebar's space menu edits (name, icon, theme,
// profile, archive delay, sync), and adding, deleting and reordering.
export class CompassSettingsSpacesPaneElement extends CrLitElement {
  static get is() {
    return 'compass-settings-spaces-pane';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      spaces_: {type: Array},
      profiles_: {type: Array},
      expandedId_: {type: String},
      themeOpen_: {type: Boolean},
      confirmingDeleteId_: {type: String},
      confirmingUnsyncId_: {type: String},
    };
  }

  // In the switcher's order.
  protected accessor spaces_: Space[] = [];
  // The profiles a space can be assigned to; none without the directory.
  protected accessor profiles_: ProfileOption[] = [];
  // The space whose details are open, and whether its theme editor is.
  protected accessor expandedId_: string|null = null;
  protected accessor themeOpen_: boolean = false;
  protected accessor confirmingDeleteId_: string|null = null;
  protected accessor confirmingUnsyncId_: string|null = null;

  protected readonly archiveDelays_: ArchiveDelay[] = ARCHIVE_DELAYS;
  protected readonly spaceSymbols_: SpaceSymbol[] = SPACE_SYMBOLS;
  private readonly handler_ =
      SettingsWindowBrowserProxy.getInstance().handler;
  private listenerId_: number|null = null;

  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    this.handler_.getSpaces().then(
        ({spaces, profiles}: {spaces: Space[], profiles: ProfileOption[]}) =>
            this.setSpaces_(spaces, profiles));
    this.listenerId_ =
        SettingsWindowBrowserProxy.getInstance()
            .callbackRouter.spacesChanged.addListener(
                (spaces: Space[], profiles: ProfileOption[]) =>
                    this.setSpaces_(spaces, profiles));
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    if (this.listenerId_ !== null) {
      SettingsWindowBrowserProxy.getInstance().callbackRouter.removeListener(
          this.listenerId_);
      this.listenerId_ = null;
    }
  }

  private setSpaces_(spaces: Space[], profiles: ProfileOption[]) {
    this.spaces_ = spaces;
    this.profiles_ = profiles;
    const has = (id: string|null) => spaces.some(space => space.id === id);
    if (!has(this.expandedId_)) {
      this.expandedId_ = null;
      this.themeOpen_ = false;
    }
    if (!has(this.confirmingDeleteId_)) {
      this.confirmingDeleteId_ = null;
    }
    if (!has(this.confirmingUnsyncId_)) {
      this.confirmingUnsyncId_ = null;
    }
  }

  // Groups -------------------------------------------------------------------

  // The profiles' spaces, each profile's in the switcher's order; profiles in
  // the profile menu's order, then any it doesn't list.
  protected getGroups_(): SpaceGroup[] {
    const groups = new Map<string, SpaceGroup>();
    for (const profile of this.profiles_) {
      groups.set(profile.id, {
        profileId: profile.id,
        profileName: profile.name,
        spaces: [],
      });
    }
    for (const space of this.spaces_) {
      let group = groups.get(space.profileId);
      if (!group) {
        group = {
          profileId: space.profileId,
          profileName: space.profileName,
          spaces: [],
        };
        groups.set(space.profileId, group);
      }
      group.spaces.push(space);
    }
    return [...groups.values()];
  }

  protected hasProfiles_(): boolean {
    return this.profiles_.length > 0;
  }

  protected getGroupTitle_(group: SpaceGroup): string {
    return this.hasProfiles_() ? group.profileName : 'Spaces';
  }

  // Space rows ---------------------------------------------------------------

  private findSpace_(id: string|null): Space|null {
    return this.spaces_.find(space => space.id === id) ?? null;
  }

  private getSpaceId_(e: Event): string {
    return (e.currentTarget as HTMLElement).dataset['spaceId']!;
  }

  protected getSpaceStyle_(space: Space): string {
    const colors = [space.color, ...space.gradient];
    const stop = (at: number) => {
      const index = Math.round(at * (colors.length - 1));
      return colorToCss(colors[index] ?? null);
    };
    return `--space-color: ${stop(0)}; --space-color-2: ${stop(0.5)}; ` +
        `--space-color-3: ${stop(1)}`;
  }

  // A space shows its symbol, its emoji, or its name's first letter.
  protected getSpaceSymbol_(space: Space): SpaceSymbol|null {
    return space.icon?.symbol ? findSpaceSymbol(space.icon.symbol) : null;
  }

  protected getSymbolViewBox_(): string {
    return SPACE_SYMBOL_VIEW_BOX;
  }

  protected getSpaceInitial_(space: Space): string {
    return Array.from(space.name.trim())[0]?.toUpperCase() ?? '•';
  }

  protected getSpaceIconText_(space: Space): string {
    return space.icon?.emoji ?? this.getSpaceInitial_(space);
  }

  protected getSpaceDetail_(space: Space): string {
    const parts = [];
    if (space.viewOnly) {
      parts.push('Shared with you, view only');
    } else if (space.shared) {
      parts.push('Shared');
    }
    if (!space.synced) {
      parts.push('On this device only');
    }
    return parts.join(' · ');
  }

  protected isExpanded_(space: Space): boolean {
    return this.expandedId_ === space.id;
  }

  protected isThemeOpen_(space: Space): boolean {
    return this.isExpanded_(space) && this.themeOpen_;
  }

  // Where the theme editor sends its edits.
  protected getHandler_(): ThemeEditorHandler {
    return this.handler_;
  }

  protected onExpandClick_(e: Event) {
    const id = this.getSpaceId_(e);
    this.expandedId_ = this.expandedId_ === id ? null : id;
    this.themeOpen_ = false;
  }

  protected onThemeClick_() {
    this.themeOpen_ = !this.themeOpen_;
  }

  // Name.
  protected onNameChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    const space = this.findSpace_(input.dataset['spaceId']!);
    if (!space) {
      return;
    }
    const name = input.value.trim();
    if (!name) {
      input.value = space.name;
      return;
    }
    if (name !== space.name) {
      this.handler_.renameSpace(space.id, name);
    }
  }

  protected onNameKeydown_(e: KeyboardEvent) {
    if (e.key === 'Enter') {
      (e.target as HTMLInputElement).blur();
    }
  }

  // Icon: |symbol| '' stands for none.
  protected isIcon_(space: Space, symbol: string): boolean {
    return symbol ? space.icon?.symbol === symbol : !space.icon;
  }

  protected onSymbolClick_(e: Event) {
    const target = e.currentTarget as HTMLElement;
    const name = target.dataset['symbol'];
    this.handler_.setSpaceIcon(
        target.dataset['spaceId']!, name ? {symbol: name} : null);
  }

  // Keeps the first character typed or picked (a whole emoji sequence).
  protected onEmojiInput_(e: Event) {
    const input = e.target as HTMLInputElement;
    const first = firstGrapheme(input.value.trim());
    input.value = first;
    if (first) {
      this.handler_.setSpaceIcon(input.dataset['spaceId']!, {emoji: first});
    }
  }

  // Profile: another, or a new one ('').
  protected onProfileChange_(e: Event) {
    const select = e.target as HTMLSelectElement;
    const space = this.findSpace_(select.dataset['spaceId']!);
    const id = select.value;
    if (!space || id === space.profileId) {
      return;
    }
    this.handler_.setSpaceProfile(space.id, id || null);
  }

  // Archive delay.
  protected onArchiveDelayChange_(e: Event) {
    const select = e.target as HTMLSelectElement;
    this.handler_.setArchiveAfter(
        select.dataset['spaceId']!, Number(select.value));
  }

  // Sync: offered while the profile's spaces sync, or for a space already
  // kept on this device. Turning it off asks first: the space leaves the
  // other devices.
  protected canChangeSync_(space: Space): boolean {
    return space.syncOn || !space.synced;
  }

  protected isConfirmingUnsync_(space: Space): boolean {
    return this.confirmingUnsyncId_ === space.id;
  }

  protected onSyncChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    const id = input.dataset['spaceId']!;
    if (input.checked) {
      this.confirmingUnsyncId_ = null;
      this.handler_.setSpaceSynced(id, true);
      return;
    }
    // Stays on until confirmed.
    input.checked = true;
    this.confirmingUnsyncId_ = id;
  }

  protected onUnsyncCancelClick_() {
    this.confirmingUnsyncId_ = null;
  }

  protected onUnsyncConfirmClick_(e: Event) {
    this.handler_.setSpaceSynced(this.getSpaceId_(e), false);
    this.confirmingUnsyncId_ = null;
  }

  // Order: up or down among the profile's spaces, which moves it past the
  // previous or next of them in the switcher.
  private getNeighbor_(space: Space, step: -1|1): Space|null {
    const own = this.spaces_.filter(other => other.profileId === space.profileId);
    return own[own.indexOf(space) + step] ?? null;
  }

  protected canMoveUp_(space: Space): boolean {
    return !!this.getNeighbor_(space, -1);
  }

  protected canMoveDown_(space: Space): boolean {
    return !!this.getNeighbor_(space, 1);
  }

  protected onMoveClick_(e: Event) {
    const target = e.currentTarget as HTMLElement;
    const space = this.findSpace_(target.dataset['spaceId']!);
    const step = target.dataset['step'] === 'up' ? -1 : 1;
    const neighbor = space && this.getNeighbor_(space, step);
    if (!space || !neighbor) {
      return;
    }
    // A slot among the switcher's spaces, counted with this one in place:
    // before the previous one, or after the next.
    const at = this.spaces_.indexOf(neighbor);
    this.handler_.moveSpace(space.id, step < 0 ? at : at + 1);
  }

  // Adding and deleting.
  protected onAddClick_(e: Event) {
    this.handler_.addSpace(
        (e.currentTarget as HTMLElement).dataset['profileId']!);
  }

  protected canDelete_(): boolean {
    return this.spaces_.length > 1;
  }

  protected isConfirmingDelete_(space: Space): boolean {
    return this.confirmingDeleteId_ === space.id;
  }

  protected getDeleteText_(space: Space): string {
    const own = this.spaces_.filter(other => other.profileId === space.profileId);
    const last = own.length === 1 && this.hasProfiles_() ?
        ` It's ${space.profileName}'s last space: the profile leaves the ` +
            'switcher, and stays in Profiles.' :
        '';
    return `Deletes “${space.name}” with its pinned and Today tabs and its ` +
        `archive.${last}`;
  }

  protected onDeleteClick_(e: Event) {
    this.confirmingDeleteId_ = this.getSpaceId_(e);
  }

  protected onDeleteCancelClick_() {
    this.confirmingDeleteId_ = null;
  }

  protected onDeleteConfirmClick_(e: Event) {
    this.handler_.deleteSpace(this.getSpaceId_(e));
    this.confirmingDeleteId_ = null;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-settings-spaces-pane': CompassSettingsSpacesPaneElement;
  }
}

customElements.define(
    CompassSettingsSpacesPaneElement.is, CompassSettingsSpacesPaneElement);
