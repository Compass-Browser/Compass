// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from '//resources/lit/v3_0/lit.rollup.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import {SidebarBrowserProxy} from './browser_proxy.js';
import './node.js';
import type {ArchivedEntry, ArchivedTab, Node, Space, Tree} from './sidebar.mojom-webui.js';
import {NodeType, SpaceColor} from './sidebar.mojom-webui.js';

// Where a dragged entry, or a dropped link, would land: the slot |index| among |parent|'s
// current children. |target| shows the drop indicator, as its `drop`
// attribute.
interface DropTarget {
  parent: Node;
  index: number;
  target: HTMLElement;
  position: 'before'|'after'|'into'|'end';
}

// What each space color looks like, and its name. Grey spaces aren't tinted.
const SPACE_COLORS: Map<SpaceColor, {css: string, name: string}> = new Map([
  [SpaceColor.kGrey, {css: 'rgb(128, 128, 128)', name: 'Grey'}],
  [SpaceColor.kBlue, {css: 'rgb(59, 130, 246)', name: 'Blue'}],
  [SpaceColor.kPurple, {css: 'rgb(139, 92, 246)', name: 'Purple'}],
  [SpaceColor.kPink, {css: 'rgb(236, 72, 153)', name: 'Pink'}],
  [SpaceColor.kRed, {css: 'rgb(239, 68, 68)', name: 'Red'}],
  [SpaceColor.kOrange, {css: 'rgb(249, 115, 22)', name: 'Orange'}],
  [SpaceColor.kYellow, {css: 'rgb(234, 179, 8)', name: 'Yellow'}],
  [SpaceColor.kGreen, {css: 'rgb(34, 197, 94)', name: 'Green'}],
]);

export interface ArchiveDelay {
  hours: number;
  label: string;
}

// The auto-archive delays the space menu offers, in hours; 0 is never.
const ARCHIVE_DELAYS: ArchiveDelay[] = [
  {hours: 12, label: '12 hours'},
  {hours: 24, label: '24 hours'},
  {hours: 7 * 24, label: '7 days'},
  {hours: 30 * 24, label: '30 days'},
  {hours: 0, label: 'Never'},
];

// How long the pointer rests on a tab before its hover card shows. Once one
// shows, moving to another tab moves it there at once. Upstream's delay.
const HOVER_CARD_DELAY_MS = 500;
// Crossing the gap between two rows doesn't hide the card.
const HOVER_CARD_HIDE_DELAY_MS = 100;

const RELATIVE_TIME = new Intl.RelativeTimeFormat(undefined, {numeric: 'auto'});

// The sidebar: the profile's favorites, as a grid of tiles, and the window's
// SpaceModel, as a pinned and a Today section. The
// browser sends the tree once, then diffs. Entries are rearranged by dragging,
// and links dragged in open where they're dropped, or onto another space's
// button to move there. A space's menu (right-click its button, or
// double-click the name) renames, colors and deletes it. Slices b, c and e of
// docs/design/sidebar.md, and docs/design/spaces.md. Today entries left
// unseen are archived; the archive button in Today's header lists them
// ("Today auto-archive").
export class StrataSidebarAppElement extends CrLitElement {
  static get is() {
    return 'strata-sidebar-app';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      favorites_: {type: Object},
      dragActive_: {type: Boolean},
      pinned_: {type: Object},
      today_: {type: Object},
      activeId_: {type: String},
      collapsedIds_: {type: Object},
      spaces_: {type: Array},
      activeSpaceId_: {type: String},
      menuSpaceId_: {type: String},
      confirmingDelete_: {type: Boolean},
      archive_: {type: Array},
      archiveOpen_: {type: Boolean},
    };
  }

  // The profile's favorites, shared by its spaces (a root of tabs).
  protected accessor favorites_: Node|null = null;
  // An entry is being dragged: the empty favorites show a drop zone.
  protected accessor dragActive_: boolean = false;
  protected accessor pinned_: Node|null = null;
  protected accessor today_: Node|null = null;
  protected accessor activeId_: string|null = null;
  protected accessor collapsedIds_: Set<string> = new Set();
  protected accessor spaces_: Space[] = [];
  protected accessor activeSpaceId_: string = '';
  // The space whose menu is open, if any.
  protected accessor menuSpaceId_: string|null = null;
  // Delete asks to be clicked twice.
  protected accessor confirmingDelete_: boolean = false;
  protected readonly spaceColors_: SpaceColor[] = Array.from(SPACE_COLORS.keys());
  protected readonly archiveDelays_ = ARCHIVE_DELAYS;
  protected accessor archive_: ArchivedEntry[] = [];
  protected accessor archiveOpen_: boolean = false;

  private browserProxy_: SidebarBrowserProxy =
      SidebarBrowserProxy.getInstance();
  private listenerIds_: number[] = [];

  // Every node of the tree, by ID, with its parent.
  private nodes_: Map<string, Node> = new Map();
  private parents_: Map<string, Node> = new Map();

  private draggingId_: string|null = null;
  private dropTarget_: DropTarget|null = null;

  // The tab the pointer rests on, whose hover card shows or is about to.
  private hoverId_: string|null = null;
  private hoverCardShown_: boolean = false;
  private hoverShowTimer_: number|null = null;
  private hoverHideTimer_: number|null = null;
  // Pressing a tab hides its card until the pointer moves to another one.
  private hoverPressedId_: string|null = null;

  override connectedCallback() {
    super.connectedCallback();
    const router = this.browserProxy_.callbackRouter;
    this.listenerIds_ = [
      router.setSpaces.addListener((spaces: Space[], activeId: string) => {
        this.spaces_ = spaces;
        this.activeSpaceId_ = activeId;
      }),
      router.setTree.addListener(this.onSetTree_.bind(this)),
      router.nodeAdded.addListener(this.onNodeAdded_.bind(this)),
      router.nodeMoved.addListener(this.onNodeMoved_.bind(this)),
      router.nodeChanged.addListener(this.onNodeChanged_.bind(this)),
      router.nodeRemoved.addListener(this.onNodeRemoved_.bind(this)),
      router.nodeLiveChanged.addListener(this.onNodeLiveChanged_.bind(this)),
      router.activeNodeChanged.addListener(
          (id: string|null) => this.activeId_ = id),
      router.setArchive.addListener(
          (entries: ArchivedEntry[]) => this.archive_ = entries),
    ];
    // Diffs that arrive before the tree are about nodes the page doesn't know
    // yet, and the lookups below drop them. The tree already has them.
    this.browserProxy_.handler.requestTree();
    document.addEventListener('dragover', this.onDocumentDragover_);
    document.addEventListener('drop', this.onDocumentDrop_);
    document.addEventListener('pointerdown', this.onDocumentPointerdown_);
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    this.hideHoverCard_();
    document.removeEventListener('dragover', this.onDocumentDragover_);
    document.removeEventListener('drop', this.onDocumentDrop_);
    document.removeEventListener('pointerdown', this.onDocumentPointerdown_);
    for (const id of this.listenerIds_) {
      this.browserProxy_.callbackRouter.removeListener(id);
    }
    this.listenerIds_ = [];
  }

  override updated(changedProperties: PropertyValues<this>) {
    super.updated(changedProperties);
    // The window's space tints the whole sidebar.
    const active = this.spaces_.find(space => this.isActiveSpace_(space));
    this.style.setProperty(
        '--space-tint',
        active && active.color !== SpaceColor.kGrey ?
            this.getSpaceColor_(active.color) :
            'transparent');
  }

  protected getActiveSpaceName_(): string {
    return this.spaces_.find(space => this.isActiveSpace_(space))?.name ?? '';
  }

  protected getSpaceColor_(color: SpaceColor): string {
    return (SPACE_COLORS.get(color) ?? SPACE_COLORS.get(SpaceColor.kGrey)!).css;
  }

  protected getColorName_(color: SpaceColor): string {
    return SPACE_COLORS.get(color)?.name ?? '';
  }

  protected getMenuSpace_(): Space|null {
    return this.spaces_.find(space => space.id === this.menuSpaceId_) ?? null;
  }

  protected isMenuSpaceColor_(color: SpaceColor): boolean {
    return this.getMenuSpace_()?.color === color;
  }

  protected canDeleteSpace_(): boolean {
    return this.spaces_.length > 1;
  }

  protected getDeleteLabel_(): string {
    return this.confirmingDelete_ ? 'Click again to delete' : 'Delete space';
  }

  private openSpaceMenu_(id: string) {
    this.menuSpaceId_ = id;
    this.confirmingDelete_ = false;
    this.updateComplete.then(() => {
      this.shadowRoot.querySelector<HTMLInputElement>('#spaceNameInput')
          ?.select();
    });
  }

  private closeSpaceMenu_() {
    this.menuSpaceId_ = null;
    this.confirmingDelete_ = false;
  }

  protected onSpaceContextmenu_(e: MouseEvent) {
    e.preventDefault();
    this.openSpaceMenu_((e.currentTarget as HTMLElement).dataset['spaceId']!);
  }

  protected onSpaceNameDblclick_() {
    this.openSpaceMenu_(this.activeSpaceId_);
  }

  private onDocumentPointerdown_ = (e: PointerEvent) => {
    const path = e.composedPath();
    const menu = this.shadowRoot.querySelector('#spaceMenu');
    if (menu && !path.includes(menu)) {
      this.closeSpaceMenu_();
    }
    // The archive button toggles the panel itself.
    const archive = this.shadowRoot.querySelector('#archive');
    const archiveButton = this.shadowRoot.querySelector('#archiveButton');
    if (archive && !path.includes(archive) &&
        !(archiveButton && path.includes(archiveButton))) {
      this.archiveOpen_ = false;
    }
  };

  protected isMenuSpaceArchiveDelay_(hours: number): boolean {
    return this.getMenuSpace_()?.archiveAfterHours === hours;
  }

  protected onArchiveDelayChange_(e: Event) {
    const hours = Number((e.target as HTMLSelectElement).value);
    if (this.menuSpaceId_) {
      this.browserProxy_.handler.setArchiveAfter(this.menuSpaceId_, hours);
    }
  }

  protected isArchiveEmpty_(): boolean {
    return this.archive_.length === 0;
  }

  protected onArchiveButtonClick_() {
    this.archiveOpen_ = !this.archiveOpen_;
  }

  protected onArchiveKeydown_(e: KeyboardEvent) {
    if (e.key === 'Escape') {
      this.archiveOpen_ = false;
    }
  }

  protected getArchiveHint_(): string {
    const hours = this.spaces_.find(space => this.isActiveSpace_(space))
                      ?.archiveAfterHours ?? 0;
    if (hours === 0) {
      return 'Nothing archived. Auto-archive is off for this space.';
    }
    const delay = ARCHIVE_DELAYS.find(delay => delay.hours === hours)?.label ??
        `${hours} hours`;
    return `Nothing archived yet. Today tabs you haven't looked at for ${
        delay} come here.`;
  }

  // A split shows its panes' titles side by side.
  protected getArchivedTitle_(entry: ArchivedEntry): string {
    return entry.tabs.map((tab: ArchivedTab) => tab.title || tab.url)
        .join(' | ');
  }

  protected getArchivedUrl_(entry: ArchivedEntry): string {
    return entry.tabs[0]?.url ?? '';
  }

  protected getArchivedAge_(entry: ArchivedEntry): string {
    const minutes = (entry.archivedAt - Date.now()) / 60000;
    if (minutes > -60) {
      return RELATIVE_TIME.format(Math.round(minutes), 'minute');
    }
    if (minutes > -24 * 60) {
      return RELATIVE_TIME.format(Math.round(minutes / 60), 'hour');
    }
    return RELATIVE_TIME.format(Math.round(minutes / (24 * 60)), 'day');
  }

  protected getFaviconUrl_(url: string, size: number = 16): string {
    const params = new URLSearchParams({
      size: `${size}`,
      scaleFactor: `${window.devicePixelRatio}x`,
      pageUrl: url,
      allowGoogleServerFallback: '0',
    });
    return `chrome://favicon2/?${params.toString()}`;
  }

  protected onArchivedClick_(e: Event) {
    this.browserProxy_.handler.restoreArchived(
        (e.currentTarget as HTMLElement).dataset['archivedId']!);
  }

  protected onRemoveArchivedClick_(e: Event) {
    e.stopPropagation();
    this.browserProxy_.handler.removeArchived(
        (e.currentTarget as HTMLElement).dataset['archivedId']!);
  }

  protected onClearArchiveClick_() {
    this.browserProxy_.handler.clearArchive();
  }

  protected onSpaceMenuKeydown_(e: KeyboardEvent) {
    if (e.key === 'Escape') {
      this.closeSpaceMenu_();
    }
  }

  protected onSpaceNameKeydown_(e: KeyboardEvent) {
    if (e.key === 'Enter') {
      this.renameMenuSpace_(e.target as HTMLInputElement);
      this.closeSpaceMenu_();
    }
  }

  protected onSpaceNameChange_(e: Event) {
    this.renameMenuSpace_(e.target as HTMLInputElement);
  }

  private renameMenuSpace_(input: HTMLInputElement) {
    const space = this.getMenuSpace_();
    const name = input.value.trim();
    if (space && name && name !== space.name) {
      this.browserProxy_.handler.renameSpace(space.id, name);
    }
  }

  protected onSwatchClick_(e: Event) {
    const color =
        Number((e.currentTarget as HTMLElement).dataset['color']) as SpaceColor;
    if (this.menuSpaceId_) {
      this.browserProxy_.handler.setSpaceColor(this.menuSpaceId_, color);
    }
  }

  protected onDeleteSpaceClick_() {
    if (!this.confirmingDelete_) {
      this.confirmingDelete_ = true;
      return;
    }
    if (this.menuSpaceId_) {
      this.browserProxy_.handler.deleteSpace(this.menuSpaceId_);
    }
    this.closeSpaceMenu_();
  }

  protected isActiveSpace_(space: Space): boolean {
    return space.id === this.activeSpaceId_;
  }

  // The switcher shows a space by its name's first letter.
  protected getSpaceInitial_(space: Space): string {
    return Array.from(space.name.trim())[0]?.toUpperCase() ?? '•';
  }

  protected onSpaceClick_(e: Event) {
    const id = (e.currentTarget as HTMLElement).dataset['spaceId']!;
    if (id !== this.activeSpaceId_) {
      this.browserProxy_.handler.switchSpace(id);
    }
  }

  protected onAddSpaceClick_() {
    this.browserProxy_.handler.addSpace();
  }

  // Entries dragged onto another space's button move there.
  protected onSpaceDragover_(e: DragEvent) {
    const button = e.currentTarget as HTMLElement;
    if (!this.draggingId_ || button.dataset['spaceId'] === this.activeSpaceId_) {
      return;
    }
    e.preventDefault();
    e.dataTransfer!.dropEffect = 'move';
    button.setAttribute('drop', '');
  }

  protected onSpaceDragleave_(e: DragEvent) {
    (e.currentTarget as HTMLElement).removeAttribute('drop');
  }

  protected onSpaceDrop_(e: DragEvent) {
    e.preventDefault();
    const id = (e.currentTarget as HTMLElement).dataset['spaceId']!;
    if (this.draggingId_ && id !== this.activeSpaceId_) {
      this.browserProxy_.handler.moveNodeToSpace(this.draggingId_, id);
    }
    this.endDrag_();
  }

  protected getFavorites_(): Node[] {
    return this.favorites_?.children ?? [];
  }

  protected isFavoritesEmpty_(): boolean {
    return this.getFavorites_().length === 0;
  }

  protected isActive_(node: Node): boolean {
    return node.id === this.activeId_;
  }

  // As for a pinned entry: closing unloads, and an unloaded one goes.
  protected getTileCloseLabel_(node: Node): string {
    return node.live ? 'Close' : 'Remove from Favorites';
  }

  protected onTileClick_(e: Event) {
    this.browserProxy_.handler.activateNode(
        (e.currentTarget as HTMLElement).dataset['nodeId']!);
  }

  protected onTileCloseClick_(e: Event) {
    e.stopPropagation();
    this.browserProxy_.handler.closeNode(
        (e.currentTarget as HTMLElement).dataset['nodeId']!);
  }

  protected getPinned_(): Node[] {
    return this.pinned_?.children ?? [];
  }

  protected getToday_(): Node[] {
    return this.today_?.children ?? [];
  }

  protected isPinnedEmpty_(): boolean {
    return this.getPinned_().length === 0;
  }

  protected onNewTabClick_() {
    this.browserProxy_.handler.newTab();
  }

  protected onNewFolderClick_() {
    if (this.pinned_) {
      this.browserProxy_.handler.addFolder(
          this.pinned_.id, this.pinned_.children.length, 'New Folder');
    }
  }

  protected onFolderToggle_(e: CustomEvent<string>) {
    this.toggleFolder_(e.detail);
  }

  private toggleFolder_(id: string) {
    const collapsed = new Set(this.collapsedIds_);
    if (!collapsed.delete(id)) {
      collapsed.add(id);
    }
    this.collapsedIds_ = collapsed;
  }

  // The rows the keyboard moves between, top to bottom: favorites' tiles,
  // then tabs (panes too) and folders, as far as they show.
  private getFocusableRows_(): HTMLElement[] {
    return Array.from(this.shadowRoot.querySelectorAll<HTMLElement>(
        '#favorites .tile, #sections .tab, #sections .folder-row'));
  }

  private getRow_(id: string): HTMLElement|null {
    return this.shadowRoot.querySelector<HTMLElement>(
        `#favorites .tile[data-node-id="${id}"], ` +
        `#sections .tab[data-node-id="${id}"], ` +
        `#sections .folder-row[data-node-id="${id}"]`);
  }

  private getSections_(): HTMLElement {
    return this.shadowRoot.querySelector<HTMLElement>('#sections')!;
  }

  // Tabbing into the sidebar lands on the active entry. While a row or a
  // tile has the focus, the list itself leaves the tab order, so Shift+Tab
  // leaves too.
  protected onSectionsFocusin_(e: FocusEvent) {
    const sections = e.currentTarget as HTMLElement;
    if (e.target !== sections) {
      sections.tabIndex = -1;
      return;
    }
    const rows = this.getFocusableRows_();
    (this.activeId_ && this.getRow_(this.activeId_) || rows[0])?.focus();
  }

  protected onFavoritesFocusin_() {
    this.getSections_().tabIndex = -1;
  }

  // Shared by the grid and the list: the focus left both.
  protected onSectionsFocusout_(e: FocusEvent) {
    const next = e.relatedTarget as globalThis.Node | null;
    const favorites = this.shadowRoot.querySelector('#favorites')!;
    if (!this.getSections_().contains(next) && !favorites.contains(next)) {
      this.getSections_().tabIndex = 0;
    }
  }

  // Arrows move in the grid, and Down from its last row on into the list.
  // Enter or Space activates a favorite, and Delete acts as its ×.
  protected onFavoritesKeydown_(e: KeyboardEvent) {
    const target = e.target as HTMLElement;
    if (!target.classList.contains('tile')) {
      return;
    }
    const rows = this.getFocusableRows_();
    const tiles = rows.filter(row => row.classList.contains('tile'));
    const index = tiles.indexOf(target);
    const id = target.dataset['nodeId'];
    if (index === -1 || !id) {
      return;
    }
    // The first row's tiles share its top.
    const columns =
        tiles.filter(tile => tile.offsetTop === tiles[0]!.offsetTop).length;
    const lastRow = Math.floor((tiles.length - 1) / columns);
    switch (e.key) {
      case 'ArrowLeft':
        tiles[Math.max(index - 1, 0)]!.focus();
        break;
      case 'ArrowRight':
        tiles[Math.min(index + 1, tiles.length - 1)]!.focus();
        break;
      case 'ArrowUp':
        tiles[index - columns]?.focus();
        break;
      case 'ArrowDown':
        if (index + columns < tiles.length) {
          tiles[index + columns]!.focus();
        } else if (Math.floor(index / columns) < lastRow) {
          // Nothing below in a shorter last row.
          tiles[tiles.length - 1]!.focus();
        } else {
          rows[tiles.length]?.focus();
        }
        break;
      case 'Home':
        rows[0]!.focus();
        break;
      case 'End':
        rows[rows.length - 1]!.focus();
        break;
      case 'Enter':
      case ' ':
        this.browserProxy_.handler.activateNode(id);
        break;
      case 'Delete':
      case 'Backspace':
        // A live favorite only unloads; an unloaded one goes away.
        if (!this.nodes_.get(id)?.live) {
          (tiles[index + 1] ?? tiles[index - 1] ?? rows[tiles.length])
              ?.focus();
        }
        this.browserProxy_.handler.closeNode(id);
        break;
      default:
        return;
    }
    e.preventDefault();
  }

  // Arrows move between rows; Left and Right also collapse, expand and go to
  // the parent folder. Enter or Space activates a tab or toggles a folder,
  // Delete closes a tab, and F2 renames a folder.
  protected onSectionsKeydown_(e: KeyboardEvent) {
    const target = e.target as HTMLElement;
    if (target.tagName === 'INPUT') {
      return;
    }
    const rows = this.getFocusableRows_();
    if (target === e.currentTarget && e.key.startsWith('Arrow')) {
      // The list itself has the focus: go to the active entry.
      (this.activeId_ && this.getRow_(this.activeId_) || rows[0])?.focus();
      e.preventDefault();
      return;
    }
    const index = rows.indexOf(target);
    const id = target.dataset['nodeId'];
    const node = id ? this.nodes_.get(id) : undefined;
    if (index === -1 || !id || !node) {
      return;
    }
    const isFolder = node.type === NodeType.kFolder;
    const collapsed = this.collapsedIds_.has(id);
    switch (e.key) {
      case 'ArrowDown':
        rows[Math.min(index + 1, rows.length - 1)]!.focus();
        break;
      case 'ArrowUp':
        rows[Math.max(index - 1, 0)]!.focus();
        break;
      case 'Home':
        rows[0]!.focus();
        break;
      case 'End':
        rows[rows.length - 1]!.focus();
        break;
      case 'ArrowRight':
        if (isFolder && collapsed) {
          this.toggleFolder_(id);
        } else if (isFolder && node.children.length > 0) {
          rows[index + 1]?.focus();
        }
        break;
      case 'ArrowLeft': {
        if (isFolder && !collapsed) {
          this.toggleFolder_(id);
          break;
        }
        // A pane's split isn't a folder; go on up.
        let parent = this.parents_.get(id);
        while (parent && parent.type === NodeType.kSplit) {
          parent = this.parents_.get(parent.id);
        }
        if (parent?.type === NodeType.kFolder) {
          this.getRow_(parent.id)?.focus();
        }
        break;
      }
      case 'Enter':
      case ' ':
        if (isFolder) {
          this.toggleFolder_(id);
        } else {
          this.browserProxy_.handler.activateNode(id);
        }
        break;
      case 'Delete':
      case 'Backspace':
        if (isFolder) {
          return;
        }
        // The row goes away; keep the focus in the list.
        (rows[index + 1] ?? rows[index - 1])?.focus();
        this.browserProxy_.handler.closeNode(id);
        break;
      case 'F2':
        if (!isFolder) {
          return;
        }
        target.closest('strata-sidebar-node')?.startRename();
        break;
      default:
        return;
    }
    e.preventDefault();
  }

  protected onSectionsPointerover_(e: PointerEvent) {
    if (e.pointerType !== 'mouse' || this.draggingId_) {
      return;
    }
    const row = (e.target as Element).closest<HTMLElement>('.tab, .tile');
    const id = row?.dataset['nodeId'] ?? null;
    if (!row || !id) {
      this.hoverId_ = null;
      this.clearHoverTimers_();
      this.hoverHideTimer_ = setTimeout(
          () => this.hideHoverCard_(), HOVER_CARD_HIDE_DELAY_MS);
      return;
    }
    if (id === this.hoverId_) {
      return;
    }
    this.hoverId_ = id;
    this.clearHoverTimers_();
    if (id === this.hoverPressedId_) {
      return;
    }
    this.hoverPressedId_ = null;
    if (this.hoverCardShown_) {
      this.showHoverCard_(row);
    } else {
      this.hoverShowTimer_ =
          setTimeout(() => this.showHoverCard_(row), HOVER_CARD_DELAY_MS);
    }
  }

  protected onSectionsPointerleave_() {
    this.hideHoverCard_();
  }

  // Clicking, dragging and scrolling hide the card.
  protected onSectionsPointerdown_() {
    this.hoverPressedId_ = this.hoverId_;
    this.hideHoverCard_();
  }

  protected onSectionsScroll_() {
    this.hideHoverCard_();
  }

  private showHoverCard_(row: HTMLElement) {
    this.hoverShowTimer_ = null;
    if (!row.isConnected) {
      return;
    }
    const rect = row.getBoundingClientRect();
    this.browserProxy_.handler.showHoverCard(
        row.dataset['nodeId']!, Math.round(rect.top), Math.round(rect.height));
    this.hoverCardShown_ = true;
  }

  private hideHoverCard_() {
    this.clearHoverTimers_();
    this.hoverId_ = null;
    if (this.hoverCardShown_) {
      this.hoverCardShown_ = false;
      this.browserProxy_.handler.hideHoverCard();
    }
  }

  private clearHoverTimers_() {
    if (this.hoverShowTimer_ !== null) {
      clearTimeout(this.hoverShowTimer_);
      this.hoverShowTimer_ = null;
    }
    if (this.hoverHideTimer_ !== null) {
      clearTimeout(this.hoverHideTimer_);
      this.hoverHideTimer_ = null;
    }
  }

  protected onDragstart_(e: DragEvent) {
    this.hideHoverCard_();
    const row = (e.target as Element).closest<HTMLElement>('.row, .tile');
    if (!row || !e.dataTransfer) {
      return;
    }
    this.draggingId_ = row.dataset['nodeId']!;
    this.dragActive_ = true;
    e.dataTransfer.effectAllowed = 'move';
    e.dataTransfer.setData('text/plain', this.draggingId_);
    row.setAttribute('dragging', '');
  }

  protected onDragover_(e: DragEvent) {
    const drop = this.findDropTarget_(e);
    this.showDropTarget_(drop);
    if (drop) {
      e.preventDefault();
      e.dataTransfer!.dropEffect = this.draggingId_ ? 'move' : 'copy';
    }
  }

  // A drop the page doesn't take would navigate the sidebar to it, so the
  // document takes every drag; only drop targets accept one.
  private onDocumentDragover_ = (e: DragEvent) => {
    if (!e.defaultPrevented) {
      e.preventDefault();
      e.dataTransfer!.dropEffect = 'none';
    }
  };

  private onDocumentDrop_ = (e: DragEvent) => {
    e.preventDefault();
  };

  protected onDragleave_(e: DragEvent) {
    const sections = e.currentTarget as HTMLElement;
    if (!sections.contains(e.relatedTarget as globalThis.Node | null)) {
      this.showDropTarget_(null);
    }
  }

  protected onDrop_(e: DragEvent) {
    e.preventDefault();
    const drop = this.dropTarget_;
    if (drop && this.draggingId_) {
      this.browserProxy_.handler.moveNode(
          this.draggingId_, drop.parent.id, drop.index);
    } else if (drop) {
      // The first URL of the list; lines starting with # are comments.
      const url = e.dataTransfer!.getData('text/uri-list')
                      .split(/\r?\n/)
                      .find(line => line && !line.startsWith('#'));
      if (url) {
        this.browserProxy_.handler.openLink(url, drop.parent.id, drop.index);
      }
    }
    this.endDrag_();
  }

  protected onDragend_() {
    this.endDrag_();
  }

  private endDrag_() {
    this.showDropTarget_(null);
    this.shadowRoot.querySelector('[dragging]')?.removeAttribute('dragging');
    this.shadowRoot.querySelector('.space[drop]')?.removeAttribute('drop');
    this.draggingId_ = null;
    this.dragActive_ = false;
  }

  private showDropTarget_(drop: DropTarget|null) {
    if (this.dropTarget_?.target !== drop?.target ||
        this.dropTarget_?.position !== drop?.position) {
      this.dropTarget_?.target.removeAttribute('drop');
      drop?.target.setAttribute('drop', drop.position);
    }
    this.dropTarget_ = drop;
  }

  // Tab rows take drops before (top half) or after (bottom half) them; folder
  // rows also take drops into them (middle third). Anywhere else in a section
  // drops at its end. Favorites' tiles take drops before (left half) or after
  // (right half) them, and the rest of the grid at its end. Returns null where the dragged entry (or link) can't
  // go; the browser checks again.
  private findDropTarget_(e: DragEvent): DropTarget|null {
    const dragged =
        this.draggingId_ ? this.nodes_.get(this.draggingId_) ?? null : null;
    const isLink = !this.draggingId_ &&
        !!e.dataTransfer?.types.includes('text/uri-list');
    if (!dragged && !isLink) {
      return null;
    }
    const element = e.target as Element;
    let drop: DropTarget|null = null;
    const row = element.closest<HTMLElement>('.row');
    const tile = element.closest<HTMLElement>('.tile');
    const grid = element.closest<HTMLElement>('#favorites');
    if (tile && this.favorites_) {
      const node = this.nodes_.get(tile.dataset['nodeId']!);
      if (!node) {
        return null;
      }
      const rect = tile.getBoundingClientRect();
      const after = e.clientX - rect.left >= rect.width / 2;
      drop = {
        parent: this.favorites_,
        index: this.favorites_.children.indexOf(node) + (after ? 1 : 0),
        target: tile,
        position: after ? 'after' : 'before',
      };
    } else if (grid && this.favorites_) {
      drop = {
        parent: this.favorites_,
        index: this.favorites_.children.length,
        target: grid,
        position: 'end',
      };
    } else if (row) {
      const node = this.nodes_.get(row.dataset['nodeId']!);
      const parent = node && this.parents_.get(node.id);
      if (!node || !parent) {
        return null;
      }
      const rect = row.getBoundingClientRect();
      const y = (e.clientY - rect.top) / rect.height;
      if (node.type === NodeType.kFolder && y > 1 / 3 && y < 2 / 3) {
        drop = {
          parent: node,
          index: node.children.length,
          target: row,
          position: 'into',
        };
      } else {
        const after = y >= 0.5;
        drop = {
          parent,
          index: parent.children.indexOf(node) + (after ? 1 : 0),
          target: row,
          position: after ? 'after' : 'before',
        };
      }
    } else {
      const section = element.closest<HTMLElement>('.section');
      const root = section?.id === 'pinned' ? this.pinned_ : this.today_;
      if (!section || !root) {
        return null;
      }
      drop = {
        parent: root,
        index: root.children.length,
        target: section,
        position: 'end',
      };
    }
    return this.canDrop_(dragged, drop.parent) ? drop : null;
  }

  // Mirrors SpaceModel::CanMove() for the cases the page can see. A link
  // (|dragged| null) goes wherever a new tab can. Favorites only take tabs.
  private canDrop_(dragged: Node|null, parent: Node): boolean {
    if (parent === this.favorites_) {
      return !dragged || dragged.type === NodeType.kTab;
    }
    for (let node: Node|undefined = parent; node;
         node = this.parents_.get(node.id)) {
      if (node === dragged) {
        return false;
      }
    }
    if (parent.type === NodeType.kSplit) {
      return false;
    }
    if (dragged?.type === NodeType.kFolder) {
      return parent === this.pinned_ || this.isInPinned_(parent);
    }
    return true;
  }

  private isInPinned_(node: Node): boolean {
    for (let n: Node|undefined = node; n; n = this.parents_.get(n.id)) {
      if (n === this.pinned_) {
        return true;
      }
    }
    return false;
  }

  private index_(node: Node, parent: Node|null) {
    this.nodes_.set(node.id, node);
    if (parent) {
      this.parents_.set(node.id, parent);
    }
    for (const child of node.children) {
      this.index_(child, node);
    }
  }

  private unindex_(node: Node) {
    this.nodes_.delete(node.id);
    this.parents_.delete(node.id);
    for (const child of node.children) {
      this.unindex_(child);
    }
  }

  private onSetTree_(tree: Tree) {
    this.nodes_.clear();
    this.parents_.clear();
    this.index_(tree.favorites, null);
    this.index_(tree.pinned, null);
    this.index_(tree.today, null);
    this.favorites_ = tree.favorites;
    this.pinned_ = tree.pinned;
    this.today_ = tree.today;
    this.activeId_ = tree.activeId;
  }

  private onNodeAdded_(parentId: string, index: number, node: Node) {
    const parent = this.nodes_.get(parentId);
    if (!parent || this.nodes_.has(node.id)) {
      return;
    }
    parent.children.splice(index, 0, node);
    this.index_(node, parent);
    this.requestUpdate();
  }

  private onNodeMoved_(id: string, newParentId: string, index: number) {
    const node = this.nodes_.get(id);
    const oldParent = this.parents_.get(id);
    const newParent = this.nodes_.get(newParentId);
    if (!node || !oldParent || !newParent) {
      return;
    }
    oldParent.children.splice(oldParent.children.indexOf(node), 1);
    newParent.children.splice(index, 0, node);
    this.parents_.set(id, newParent);
    this.requestUpdate();
  }

  private onNodeChanged_(id: string, title: string, url: string) {
    const node = this.nodes_.get(id);
    if (!node) {
      return;
    }
    node.title = title;
    node.url = url;
    this.requestUpdate();
  }

  private onNodeLiveChanged_(id: string, live: boolean) {
    const node = this.nodes_.get(id);
    if (node) {
      node.live = live;
      this.requestUpdate();
    }
  }

  private onNodeRemoved_(id: string) {
    if (id === this.hoverId_) {
      this.hideHoverCard_();
    }
    const node = this.nodes_.get(id);
    const parent = this.parents_.get(id);
    if (!node || !parent) {
      return;
    }
    parent.children.splice(parent.children.indexOf(node), 1);
    this.unindex_(node);
    this.requestUpdate();
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'strata-sidebar-app': StrataSidebarAppElement;
  }
}

customElements.define(StrataSidebarAppElement.is, StrataSidebarAppElement);
