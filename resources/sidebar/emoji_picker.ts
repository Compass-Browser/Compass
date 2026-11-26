// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from '//resources/lit/v3_0/lit.rollup.js';
import {isWindows} from '//resources/js/platform.js';

import type {EmojiGroupData} from './emoji_data.js';
import {getCss} from './emoji_picker.css.js';
import {getHtml} from './emoji_picker.html.js';
import {firstGrapheme} from './space_options.js';

export interface Emoji {
  emoji: string;
  name: string;
  // The name and keywords, lower case, for search.
  words: string[];
}

export interface EmojiGroup {
  id: string;
  label: string;
  icon: string;
  emoji: Emoji[];
}

// The most search results shown.
const MAX_RESULTS = 120;
// Text that is an emoji itself (typed with the system's picker, or pasted)
// rather than words to search for.
const EMOJI_TEXT = /\p{Extended_Pictographic}|\p{Regional_Indicator}/u;

// emoji_data.ts, parsed once for every picker of the page. It's imported
// when a picker first opens, so pages that don't open one never load it.
let groupsPromise: Promise<EmojiGroup[]>|null = null;

function loadGroups(): Promise<EmojiGroup[]> {
  groupsPromise ??= import('./emoji_data.js').then(
      ({EMOJI_GROUPS}) => EMOJI_GROUPS
          // Windows has no flag emoji: they'd show as two letters.
          .filter((group: EmojiGroupData) => !isWindows || group.id !== 'flags')
          .map((group: EmojiGroupData) => ({
                 id: group.id,
                 label: group.label,
                 icon: group.icon,
                 emoji: group.emoji.split('\n').map(line => {
                   const [emoji, name, keywords] = line.split('\t');
                   return {
                     emoji: emoji!,
                     name: name!,
                     words: `${name} ${keywords}`.toLowerCase()
                                .split(/[\s:,]+/)
                                .filter(word => word),
                   };
                 }),
               })));
  return groupsPromise;
}

// Whether every word of |query| starts a word of |emoji|'s.
function matches(emoji: Emoji, query: string[]): boolean {
  return query.every(q => emoji.words.some(word => word.startsWith(q)));
}

// Picks an emoji for a space's icon (docs/design/spaces.md, "Space icons"):
// a search field, the groups as tabs, and the group's emoji, or the search's
// results across all of them. Fires 'emoji-pick' with {emoji}. Enter picks
// the first result; an emoji typed or pasted in the field (macOS's Fn E
// picker types there) is a result of its own.
export class CompassEmojiPickerElement extends CrLitElement {
  static get is() {
    return 'compass-emoji-picker';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      // The space's emoji, shown as picked.
      selected: {type: String},
      groups_: {type: Array},
      groupId_: {type: String},
      query_: {type: String},
      shown_: {type: Array},
    };
  }

  accessor selected: string = '';
  // Empty until emoji_data.js has loaded.
  protected accessor groups_: EmojiGroup[] = [];
  protected accessor groupId_: string = '';
  protected accessor query_: string = '';
  // The group's emoji, or the search's results.
  protected accessor shown_: Emoji[] = [];

  override connectedCallback() {
    super.connectedCallback();
    loadGroups().then(groups => {
      this.groups_ = groups;
      this.groupId_ ||= this.findGroupOf_(this.selected) ?? groups[0]!.id;
    });
  }

  override willUpdate(changed: PropertyValues<this>) {
    super.willUpdate(changed);
    const changedPrivate = changed as Map<PropertyKey, unknown>;
    if (changedPrivate.has('groups_') || changedPrivate.has('groupId_') ||
        changedPrivate.has('query_')) {
      this.shown_ = this.findShown_();
    }
  }

  override firstUpdated(changed: PropertyValues<this>) {
    super.firstUpdated(changed);
    this.focusSearch();
  }

  focusSearch() {
    this.shadowRoot.querySelector<HTMLInputElement>('#search')?.focus();
  }

  // The group's emoji, or the search's results across all of them.
  private findShown_(): Emoji[] {
    const query = this.query_.trim();
    if (!query) {
      return this.groups_.find(group => group.id === this.groupId_)?.emoji ??
          [];
    }
    if (EMOJI_TEXT.test(query)) {
      const emoji = firstGrapheme(query);
      return [this.findEmoji_(emoji) ?? {emoji, name: emoji, words: []}];
    }
    const words = query.toLowerCase().split(/[\s:,]+/).filter(word => word);
    const results: Emoji[] = [];
    // Names that start with the query first, then any match.
    const all = this.groups_.flatMap(group => group.emoji);
    const first = words[0]!;
    for (const emoji of all) {
      if (emoji.name.toLowerCase().startsWith(first) && matches(emoji, words)) {
        results.push(emoji);
      }
    }
    for (const emoji of all) {
      if (results.length >= MAX_RESULTS) {
        break;
      }
      if (!results.includes(emoji) && matches(emoji, words)) {
        results.push(emoji);
      }
    }
    return results.slice(0, MAX_RESULTS);
  }

  protected isSearching_(): boolean {
    return !!this.query_.trim();
  }

  protected isLoading_(): boolean {
    return this.groups_.length === 0;
  }

  protected isGroup_(group: EmojiGroup): boolean {
    return !this.isSearching_() && group.id === this.groupId_;
  }

  protected isSelected_(emoji: Emoji): boolean {
    return emoji.emoji === this.selected;
  }

  protected onSearchInput_(e: Event) {
    this.query_ = (e.target as HTMLInputElement).value;
    this.shadowRoot.querySelector('#grid')?.scrollTo(0, 0);
  }

  protected onSearchKeydown_(e: KeyboardEvent) {
    if (e.key === 'Enter') {
      const first = this.shown_[0];
      if (first) {
        this.pick_(first.emoji);
      }
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      this.focusCell_(0);
    } else if (e.key === 'Escape' && this.query_) {
      // Clears the search first; the next Esc is the host's.
      e.stopPropagation();
      this.query_ = '';
      (e.target as HTMLInputElement).value = '';
    }
  }

  protected onGroupClick_(e: Event) {
    this.groupId_ = (e.currentTarget as HTMLElement).dataset['group']!;
    this.query_ = '';
    const search = this.shadowRoot.querySelector<HTMLInputElement>('#search');
    if (search) {
      search.value = '';
    }
    this.shadowRoot.querySelector('#grid')?.scrollTo(0, 0);
  }

  protected onEmojiClick_(e: Event) {
    this.pick_((e.currentTarget as HTMLElement).dataset['emoji']!);
  }

  // Arrows move between the emoji; up from the first row goes back to the
  // search field.
  protected onGridKeydown_(e: KeyboardEvent) {
    const cells = this.getCells_();
    const index = cells.indexOf(e.target as HTMLElement);
    if (index === -1) {
      return;
    }
    const columns = this.getColumns_(cells);
    const step: Record<string, number> = {
      ArrowLeft: -1,
      ArrowRight: 1,
      ArrowUp: -columns,
      ArrowDown: columns,
    };
    if (!(e.key in step)) {
      return;
    }
    e.preventDefault();
    const next = index + step[e.key]!;
    if (next < 0 && e.key === 'ArrowUp') {
      this.focusSearch();
    } else if (next >= 0 && next < cells.length) {
      this.focusCell_(next);
    }
  }

  private getCells_(): HTMLElement[] {
    return Array.from(
        this.shadowRoot.querySelectorAll<HTMLElement>('#grid .emoji'));
  }

  // How many emoji a row holds: those on the first one.
  private getColumns_(cells: HTMLElement[]): number {
    const top = cells[0]?.offsetTop;
    const columns = cells.findIndex(cell => cell.offsetTop !== top);
    return columns === -1 ? cells.length : columns;
  }

  private focusCell_(index: number) {
    this.getCells_()[index]?.focus();
  }

  private pick_(emoji: string) {
    this.fire('emoji-pick', {emoji});
  }

  private findEmoji_(text: string): Emoji|undefined {
    const bare = text.replace(/️/g, '');
    for (const group of this.groups_) {
      const found = group.emoji.find(
          emoji => emoji.emoji.replace(/️/g, '') === bare);
      if (found) {
        return found;
      }
    }
    return undefined;
  }

  private findGroupOf_(text: string): string|undefined {
    if (!text) {
      return undefined;
    }
    const bare = text.replace(/️/g, '');
    return this.groups_
        .find(group => group.emoji.some(
                  emoji => emoji.emoji.replace(/️/g, '') === bare))
        ?.id;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-emoji-picker': CompassEmojiPickerElement;
  }
}

customElements.define(
    CompassEmojiPickerElement.is, CompassEmojiPickerElement);
