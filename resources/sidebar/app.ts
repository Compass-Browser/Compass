// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';
import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import {SidebarBrowserProxy} from './browser_proxy.js';
import {getFaviconUrl} from './favicon.js';
import './node.js';
import type {CompassSidebarPreviewElement} from './preview.js';
import './preview.js';
import type {SpaceSymbol} from './space_icons.js';
import {findSpaceSymbol, SPACE_SYMBOL_VIEW_BOX, SPACE_SYMBOLS} from './space_icons.js';
import type {ArchivedEntry, ArchivedTab, Node, ProfileOption, Space, SpaceColor, Tree} from './sidebar.mojom-webui.js';
import {NodeType} from './sidebar.mojom-webui.js';

// Where a dragged entry, or a dropped link, would land: the slot |index| among |parent|'s
// current children. |target| shows the drop indicator, as its `drop`
// attribute.
// A split drop has the tab dropped on as |parent|.
interface DropTarget {
  parent: Node;
  index: number;
  target: HTMLElement;
  position: 'before'|'after'|'into'|'end'|'split-left'|'split-right';
}

// The space menu's palette, as SidebarTint (sidebar_tint.cc) tints the region
// with it (compass::SpaceColor's constants). Grey (null) isn't tinted. Any
// other color can be picked too (docs/design/spaces.md, "Space colors").
export interface PaletteColor {
  color: SpaceColor|null;
  name: string;
}

const SPACE_COLORS: PaletteColor[] = [
  {color: null, name: 'Grey'},
  {color: {red: 59, green: 130, blue: 246}, name: 'Blue'},
  {color: {red: 139, green: 92, blue: 246}, name: 'Purple'},
  {color: {red: 236, green: 72, blue: 153}, name: 'Pink'},
  {color: {red: 239, green: 68, blue: 68}, name: 'Red'},
  {color: {red: 249, green: 115, blue: 22}, name: 'Orange'},
  {color: {red: 234, green: 179, blue: 8}, name: 'Yellow'},
  {color: {red: 34, green: 197, blue: 94}, name: 'Green'},
];

function firstGrapheme(text: string): string {
  const segments = new Intl.Segmenter(undefined, {granularity: 'grapheme'});
  for (const {segment} of segments.segment(text)) {
    return segment;
  }
  return '';
}

// A space's color and its gradient colors, at most (kMaxSpaceGradientColors).
const MAX_GRADIENT_COLORS = 3;

// |color| with its hue turned |degrees|, as HSL.
function rotateHue(color: SpaceColor, degrees: number): SpaceColor {
  const r = color.red / 255;
  const g = color.green / 255;
  const b = color.blue / 255;
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const light = (max + min) / 2;
  const chroma = max - min;
  let hue = 0;
  if (chroma) {
    hue = max === r ? ((g - b) / chroma + 6) % 6 :
        max === g   ? (b - r) / chroma + 2 :
                      (r - g) / chroma + 4;
  }
  const saturation = chroma ? chroma / (1 - Math.abs(2 * light - 1)) : 0;
  const h = ((hue * 60 + degrees) % 360 + 360) % 360;
  const c = (1 - Math.abs(2 * light - 1)) * saturation;
  const x = c * (1 - Math.abs((h / 60) % 2 - 1));
  const m = light - c / 2;
  const [r1, g1, b1] = h < 60 ? [c, x, 0] :
      h < 120                ? [x, c, 0] :
      h < 180                ? [0, c, x] :
      h < 240                ? [0, x, c] :
      h < 300                ? [x, 0, c] :
                               [c, 0, x];
  const byte = (v: number) => Math.round((v + m) * 255);
  return {red: byte(r1), green: byte(g1), blue: byte(b1)};
}

// How often a custom color being dragged in the picker is passed on.
const CUSTOM_COLOR_MS = 50;

function sameColor(a: SpaceColor|null, b: SpaceColor|null): boolean {
  return a === b ||
      (!!a && !!b && a.red === b.red && a.green === b.green &&
       a.blue === b.blue);
}

function colorToCss(color: SpaceColor|null): string {
  return color ? `rgb(${color.red}, ${color.green}, ${color.blue})` :
                 'rgb(128, 128, 128)';
}

// As <input type="color"> wants it.
function colorToHex(color: SpaceColor|null): string {
  const c = color ?? {red: 128, green: 128, blue: 128};
  return '#' +
      [c.red, c.green, c.blue]
          .map(component => component.toString(16).padStart(2, '0'))
          .join('');
}

function hexToColor(hex: string): SpaceColor|null {
  const match = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex);
  return match ? {
    red: parseInt(match[1]!, 16),
    green: parseInt(match[2]!, 16),
    blue: parseInt(match[3]!, 16),
  } :
                 null;
}

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

// The drag data of a space button dragged along the switcher.
const SPACE_DRAG_TYPE = 'application/x-compass-space';

// Entries glide to where they moved, and fade in and out
// (docs/design/sidebar.md, "Motion").
const FLIP_MS = 220;
const FADE_MS = 160;
const EASE_OUT = 'cubic-bezier(0.2, 0, 0, 1)';

// Switching spaces slides the old space's entries out and the new one's in,
// while the tint fades (SidebarTint's kFadeDuration). A two-finger swipe
// moves them with the fingers (docs/design/spaces.md, "Swiping").
const SLIDE_MS = 280;
// Wheel events this far apart end a gesture. Momentum after the fingers lift
// keeps it going.
const WHEEL_QUIET_MS = 150;
// Of the sidebar's width: a swipe this far switches at once; one that stops
// short of the other switches if it's past the second, and goes back if not.
const SWIPE_SWITCH = 0.5;
const SWIPE_SETTLE = 0.25;
// Past the first or the last space, the entries only give a little.
const SWIPE_RESISTANCE = 0.2;
const SWIPE_RESISTANCE_MAX_PX = 32;
// A switch the browser hasn't confirmed by then didn't happen.
const SWITCH_TIMEOUT_MS = 2000;

// A two-finger swipe across the sidebar.
interface Swipe {
  // How far the fingers moved the entries, in pixels; negative pulls in the
  // next space.
  offset: number;
  // The space pulled in: the next (1), the previous (-1) or none yet (0).
  direction: number;
  // Null past the first or the last space.
  target: Space|null;
}

// A switch to another space, as it slides.
interface Slide {
  // Where the new space comes from: the right (1) or the left (-1).
  direction: number;
  // The page slid the new space's preview in before the browser switched (a
  // swipe, or a click on a space button).
  fromPreview: boolean;
  // The browser has switched; the tint is its own again.
  switched: boolean;
  // The tree for the new space has arrived.
  hasTree: boolean;
  animations: Animation[];
}

// A space next to the window's, built off to the side for a swipe to pull
// in.
export interface Preview {
  space: Space;
  // Null until it arrives: only the name shows.
  tree: Tree|null;
  side: 'previous'|'next';
}

// Which way the wheel events of a gesture go. `done`: the swipe is over, and
// the rest of the gesture (its momentum) is swallowed.
type WheelAxis = 'x'|'y'|'done';

// Where an animated element was before a change, and the element.
interface FlipFirst {
  rect: DOMRect;
  element: HTMLElement;
}

// The sidebar: the profile's favorites, as a grid of tiles, and the window's
// SpaceModel, as a pinned and a Today section. The
// browser sends the tree once, then diffs. Entries are rearranged by dragging,
// and links dragged in open where they're dropped, or onto another space's
// button to move there. A space's menu (right-click its button, or
// double-click the name) renames, colors and deletes it. Slices b, c and e of
// docs/design/sidebar.md, and docs/design/spaces.md. Today entries left
// unseen are archived; the archive button in Today's header lists them
// ("Today auto-archive").
export class CompassSidebarAppElement extends CrLitElement {
  static get is() {
    return 'compass-sidebar-app';
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
      spaces_: {type: Array},
      profiles_: {type: Array},
      activeSpaceId_: {type: String},
      menuSpaceId_: {type: String},
      confirmingDelete_: {type: Boolean},
      archive_: {type: Array},
      archiveOpen_: {type: Boolean},
      previews_: {type: Array},
    };
  }

  // The profile's favorites, shared by its spaces (a root of tabs).
  protected accessor favorites_: Node|null = null;
  // An entry is being dragged: the empty favorites show a drop zone.
  protected accessor dragActive_: boolean = false;
  protected accessor pinned_: Node|null = null;
  protected accessor today_: Node|null = null;
  protected accessor activeId_: string|null = null;
  protected accessor spaces_: Space[] = [];
  // The profiles a space can be assigned to; none off the record.
  protected accessor profiles_: ProfileOption[] = [];
  protected accessor activeSpaceId_: string = '';
  // The space whose menu is open, if any.
  protected accessor menuSpaceId_: string|null = null;
  // Delete asks to be clicked twice.
  protected accessor confirmingDelete_: boolean = false;
  // The color picked last in a color picker, sent when the timer fires.
  private pendingColorSend_: (() => void)|null = null;
  private colorTimer_: number|null = null;
  protected readonly spaceColors_: PaletteColor[] = SPACE_COLORS;
  protected readonly spaceSymbols_: SpaceSymbol[] = SPACE_SYMBOLS;
  protected readonly archiveDelays_ = ARCHIVE_DELAYS;
  protected accessor archive_: ArchivedEntry[] = [];
  protected accessor archiveOpen_: boolean = false;
  // The spaces before and after the window's, kept built so a swipe shows
  // them at once. They change only between slides.
  protected accessor previews_: Preview[] = [];

  private browserProxy_: SidebarBrowserProxy =
      SidebarBrowserProxy.getInstance();
  private listenerIds_: number[] = [];

  // Every node of the tree, by ID, with its parent.
  private nodes_: Map<string, Node> = new Map();
  private parents_: Map<string, Node> = new Map();

  private draggingId_: string|null = null;
  private dropTarget_: DropTarget|null = null;
  // A space button dragged along the switcher, and the slot it would drop
  // in, counted with it in place.
  private draggingSpaceId_: string|null = null;
  private spaceDropIndex_: number|null = null;

  // The tab the pointer rests on, whose hover card shows or is about to.
  private hoverId_: string|null = null;
  private hoverCardShown_: boolean = false;
  private hoverShowTimer_: number|null = null;
  private hoverHideTimer_: number|null = null;
  // Pressing a tab hides its card until the pointer moves to another one.
  private hoverPressedId_: string|null = null;

  // Where the entries were before the change being rendered, by node ID or
  // `data-flip`, while their animation waits for the next frame.
  private flipFirst_: Map<string, FlipFirst>|null = null;
  private flipAnimations_: Animation[] = [];

  private wheelAxis_: WheelAxis|null = null;
  private wheelQuietTimer_: number|null = null;
  private swipe_: Swipe|null = null;
  private swipeFrame_: number|null = null;
  // Other spaces' trees, for the previews, by space ID. Refetched whenever
  // the window's space or the spaces change.
  private spaceTrees_: Map<string, Tree> = new Map();
  // A switch the browser was asked for, or told the page about.
  private slide_: Slide|null = null;
  private slideTimer_: number|null = null;
  // The old space's entries, as they slide out.
  private slideGhost_: HTMLElement|null = null;
  // A swipe that stopped short, going back.
  private settleAnimations_: Animation[] = [];

  override connectedCallback() {
    super.connectedCallback();
    // The window's theme follows its space (docs/design/spaces.md, "Window
    // theme"): reload colors.css when it changes.
    ColorChangeUpdater.forDocument().start();
    const router = this.browserProxy_.callbackRouter;
    this.listenerIds_ = [
      router.setSpaces.addListener(this.onSetSpaces_.bind(this)),
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
      router.startRename.addListener((id: string) => {
        this.getRow_(id)?.closest('compass-sidebar-node')?.startRename(id);
      }),
    ];
    // Diffs that arrive before the tree are about nodes the page doesn't know
    // yet, and the lookups below drop them. The tree already has them.
    this.browserProxy_.handler.requestTree();
    document.addEventListener('dragover', this.onDocumentDragover_);
    document.addEventListener('drop', this.onDocumentDrop_);
    document.addEventListener('pointerdown', this.onDocumentPointerdown_);
    document.addEventListener('contextmenu', this.onDocumentContextmenu_);
    this.addEventListener('wheel', this.onWheel_, {passive: false});
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    this.hideHoverCard_();
    document.removeEventListener('dragover', this.onDocumentDragover_);
    document.removeEventListener('drop', this.onDocumentDrop_);
    document.removeEventListener('pointerdown', this.onDocumentPointerdown_);
    document.removeEventListener('contextmenu', this.onDocumentContextmenu_);
    this.removeEventListener('wheel', this.onWheel_);
    this.clearWheelQuietTimer_();
    if (this.swipeFrame_ !== null) {
      cancelAnimationFrame(this.swipeFrame_);
      this.swipeFrame_ = null;
    }
    if (this.swipe_) {
      this.browserProxy_.handler.previewSpace(null, 0);
      this.swipe_ = null;
    }
    this.endSlide_();
    for (const id of this.listenerIds_) {
      this.browserProxy_.callbackRouter.removeListener(id);
    }
    this.listenerIds_ = [];
  }

  protected getActiveSpaceName_(): string {
    return this.spaces_.find(space => this.isActiveSpace_(space))?.name ?? '';
  }

  protected getSpaceColor_(color: SpaceColor|null): string {
    return colorToCss(color);
  }

  protected getMenuSpace_(): Space|null {
    return this.spaces_.find(space => space.id === this.menuSpaceId_) ?? null;
  }

  protected isMenuSpaceColor_(color: SpaceColor|null): boolean {
    const space = this.getMenuSpace_();
    return !!space && sameColor(space.color, color);
  }

  // Whether the menu's space has a color outside the palette.
  protected isMenuSpaceCustomColor_(): boolean {
    const space = this.getMenuSpace_();
    return !!space &&
        !SPACE_COLORS.some(entry => sameColor(entry.color, space.color));
  }

  protected getMenuSpaceHex_(): string {
    return colorToHex(this.getMenuSpace_()?.color ?? null);
  }

  protected getColorHex_(color: SpaceColor): string {
    return colorToHex(color);
  }

  // A space button's colors: its gradient's three stops, all its color for
  // a plain one, as the sidebar's tint samples them (sidebar_tint.cc).
  protected getSpaceStyle_(space: Space): string {
    const colors = [space.color, ...space.gradient];
    const stop = (at: number) => {
      const index = Math.round(at * (colors.length - 1));
      return colorToCss(colors[index] ?? null);
    };
    return `--space-color: ${stop(0)}; --space-color-2: ${stop(0.5)}; ` +
        `--space-color-3: ${stop(1)}`;
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
    return getFaviconUrl(url, size, this.getProfileId_());
  }

  // The window's profile, whose entries the live pane shows: after a switch
  // to another profile's space, not the page's own.
  protected getProfileId_(): string|null {
    return this.spaces_.find(space => space.id === this.activeSpaceId_)
               ?.profileId ??
        null;
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
    const entry = SPACE_COLORS[Number(
        (e.currentTarget as HTMLElement).dataset['index'])];
    if (entry && this.menuSpaceId_) {
      this.browserProxy_.handler.setSpaceColor(this.menuSpaceId_, entry.color);
    }
  }

  protected onCustomColorInput_(e: Event) {
    const color = hexToColor((e.target as HTMLInputElement).value);
    const spaceId = this.menuSpaceId_;
    if (color && spaceId) {
      this.sendColorSoon_(
          () => this.browserProxy_.handler.setSpaceColor(spaceId, color));
    }
  }

  protected onGradientColorInput_(e: Event) {
    const input = e.target as HTMLInputElement;
    const color = hexToColor(input.value);
    const space = this.getMenuSpace_();
    if (!color || !space) {
      return;
    }
    const gradient = [...space.gradient];
    gradient[Number(input.dataset['index'])] = color;
    this.sendColorSoon_(
        () => this.browserProxy_.handler.setSpaceGradient(space.id, gradient));
  }

  protected onRemoveGradientColorClick_(e: Event) {
    const space = this.getMenuSpace_();
    if (space) {
      const index = Number((e.currentTarget as HTMLElement).dataset['index']);
      this.browserProxy_.handler.setSpaceGradient(
          space.id, space.gradient.filter((_, i) => i !== index));
    }
  }

  // The new color goes on from the last one, a little further round the hue
  // wheel.
  protected onAddGradientColorClick_() {
    const space = this.getMenuSpace_();
    if (space?.color && this.canAddGradientColor_()) {
      const last = space.gradient[space.gradient.length - 1] ?? space.color;
      this.browserProxy_.handler.setSpaceGradient(
          space.id, [...space.gradient, rotateHue(last, 40)]);
    }
  }

  protected canAddGradientColor_(): boolean {
    const space = this.getMenuSpace_();
    return !!space?.color && space.gradient.length < MAX_GRADIENT_COLORS - 1;
  }

  // The color picker sends a color for every move while dragging: pass on at
  // most one every CUSTOM_COLOR_MS, the last, as the window follows each.
  private sendColorSoon_(send: () => void) {
    this.pendingColorSend_ = send;
    if (this.colorTimer_ !== null) {
      return;
    }
    this.colorTimer_ = setTimeout(() => {
      this.colorTimer_ = null;
      const pending = this.pendingColorSend_;
      this.pendingColorSend_ = null;
      pending?.();
    }, CUSTOM_COLOR_MS);
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
  // Whether the switcher's spaces use more than one profile.
  protected hasOtherProfiles_(): boolean {
    return this.spaces_.some(space => !space.ownProfile);
  }

  protected getSpaceTitle_(space: Space): string {
    return this.hasOtherProfiles_() ?
        `${space.name} · ${space.profileName}` :
        space.name;
  }

  protected isMenuSpaceProfile_(profile: ProfileOption): boolean {
    return this.getMenuSpace_()?.profileId === profile.id;
  }

  // Assigns the menu's space to the chosen profile, or a new one ('').
  protected onSpaceProfileChange_(e: Event) {
    const space = this.getMenuSpace_();
    const id = (e.target as HTMLSelectElement).value;
    if (!space || id === space.profileId) {
      return;
    }
    this.browserProxy_.handler.setSpaceProfile(space.id, id || null);
    this.closeSpaceMenu_();
  }

  protected getSpaceInitial_(space: Space): string {
    return Array.from(space.name.trim())[0]?.toUpperCase() ?? '•';
  }

  // A space shows its symbol, its emoji, or its name's first letter
  // (docs/design/spaces.md, "Space icons"). An unknown symbol (from a later
  // version) shows the letter.
  protected getSpaceSymbol_(space: Space): SpaceSymbol|null {
    return space.icon?.symbol ? findSpaceSymbol(space.icon.symbol) : null;
  }

  protected getSymbolViewBox_(): string {
    return SPACE_SYMBOL_VIEW_BOX;
  }

  protected getSpaceIconText_(space: Space): string {
    return space.icon?.emoji ?? this.getSpaceInitial_(space);
  }

  protected getSpaceIconClass_(space: Space): string {
    return space.icon?.emoji ? 'space-emoji' : 'space-initial';
  }

  // |symbol| '' stands for no icon.
  protected isMenuSpaceIcon_(symbol: string): boolean {
    const icon = this.getMenuSpace_()?.icon;
    return symbol ? icon?.symbol === symbol : !icon;
  }

  protected onSymbolClick_(e: Event) {
    const name = (e.currentTarget as HTMLElement).dataset['symbol'];
    if (this.menuSpaceId_) {
      this.browserProxy_.handler.setSpaceIcon(
          this.menuSpaceId_, name ? {symbol: name} : null);
    }
  }

  // Keeps the first character typed or picked (a whole emoji sequence).
  protected onEmojiInput_(e: Event) {
    const input = e.target as HTMLInputElement;
    const first = firstGrapheme(input.value.trim());
    input.value = first;
    if (first && this.menuSpaceId_) {
      this.browserProxy_.handler.setSpaceIcon(this.menuSpaceId_, {emoji: first});
    }
  }

  // Slides the space's preview in at once, as a swipe would, rather than
  // after the browser has switched.
  protected async onSpaceClick_(e: Event) {
    const id = (e.currentTarget as HTMLElement).dataset['spaceId']!;
    const target = this.spaces_.find(space => space.id === id);
    if (!target || id === this.activeSpaceId_) {
      return;
    }
    const from = this.spaces_.findIndex(space => this.isActiveSpace_(space));
    const direction = this.spaces_.indexOf(target) > from ? 1 : -1;
    if (this.swipe_ || this.slide_ || from === -1 || !this.motionAllowed_()) {
      this.browserProxy_.handler.switchSpace(id);
      return;
    }
    if (this.getNeighborSpace_(direction) !== target) {
      // Further away: its preview takes the neighbor's place.
      const side = direction > 0 ? 'next' : 'previous';
      this.previews_ = [
        ...this.previews_.filter(preview => preview.side !== side),
        {space: target, side, tree: this.spaceTrees_.get(id) ?? null},
      ];
      await this.updateComplete;
      await (this.getPreview_(direction) as CrLitElement | null)
          ?.updateComplete;
    }
    const preview = this.getPreview_(direction) as
        CompassSidebarPreviewElement | null;
    if (this.swipe_ || this.slide_ || id === this.activeSpaceId_ ||
        preview?.space?.id !== id) {
      this.browserProxy_.handler.switchSpace(id);
      return;
    }
    this.slideTo_(target, direction, preview, 0);
  }

  protected onAddSpaceClick_() {
    this.browserProxy_.handler.addSpace();
  }

  // Entries dragged onto another space's button move there, but not to
  // another profile's space: their tabs can't.
  protected isDropSpace_(id: string): boolean {
    return id !== this.activeSpaceId_ &&
        !!this.spaces_.find(space => space.id === id)?.ownProfile;
  }

  protected onSpaceDragstart_(e: DragEvent) {
    this.hideHoverCard_();
    const button = e.currentTarget as HTMLElement;
    if (!e.dataTransfer) {
      return;
    }
    this.draggingSpaceId_ = button.dataset['spaceId']!;
    e.dataTransfer.effectAllowed = 'move';
    e.dataTransfer.setData(SPACE_DRAG_TYPE, this.draggingSpaceId_);
    button.setAttribute('dragging', '');
  }

  // A space dragged over a button goes before it (its leading half) or after
  // it. Its own slots show nothing.
  private showSpaceDrop_(e: DragEvent, button: HTMLElement) {
    const index =
        this.spaces_.findIndex(space => space.id === button.dataset['spaceId']);
    const from =
        this.spaces_.findIndex(space => space.id === this.draggingSpaceId_);
    const rect = button.getBoundingClientRect();
    const rtl = getComputedStyle(button).direction === 'rtl';
    const after = (e.clientX - rect.left >= rect.width / 2) !== rtl;
    const slot = index + (after ? 1 : 0);
    this.clearSpaceDrop_();
    if (index !== -1 && from !== -1 && slot !== from && slot !== from + 1) {
      button.setAttribute('reorder', after ? 'after' : 'before');
      this.spaceDropIndex_ = slot;
    }
  }

  private clearSpaceDrop_() {
    this.shadowRoot.querySelector('.space[reorder]')?.removeAttribute(
        'reorder');
    this.spaceDropIndex_ = null;
  }

  protected onSpaceDragover_(e: DragEvent) {
    const button = e.currentTarget as HTMLElement;
    if (this.draggingSpaceId_) {
      e.preventDefault();
      e.dataTransfer!.dropEffect = 'move';
      this.showSpaceDrop_(e, button);
      return;
    }
    if (!this.draggingId_ || !this.isDropSpace_(button.dataset['spaceId']!)) {
      return;
    }
    e.preventDefault();
    e.dataTransfer!.dropEffect = 'move';
    button.setAttribute('drop', '');
  }

  protected onSpaceDragleave_(e: DragEvent) {
    const button = e.currentTarget as HTMLElement;
    button.removeAttribute('drop');
    if (button.hasAttribute('reorder')) {
      this.clearSpaceDrop_();
    }
  }

  protected onSpaceDrop_(e: DragEvent) {
    e.preventDefault();
    const id = (e.currentTarget as HTMLElement).dataset['spaceId']!;
    if (this.draggingSpaceId_ && this.spaceDropIndex_ !== null) {
      this.browserProxy_.handler.moveSpace(
          this.draggingSpaceId_, this.spaceDropIndex_);
    } else if (this.draggingId_ && this.isDropSpace_(id)) {
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

  // At the end of Pinned, or the top of Today, where new entries go.
  protected onNewFolderClick_(e: Event) {
    const today = (e.currentTarget as HTMLElement).dataset['section'] ===
        'today';
    const root = today ? this.today_ : this.pinned_;
    if (root) {
      this.browserProxy_.handler.addFolder(
          root.id, today ? 0 : root.children.length, 'New Folder');
    }
  }

  protected onFolderToggle_(e: CustomEvent<string>) {
    this.toggleFolder_(e.detail);
  }

  // Shown at once, then saved by the browser, which tells the other windows.
  private toggleFolder_(id: string) {
    const node = this.nodes_.get(id);
    if (!node || node.type !== NodeType.kFolder) {
      return;
    }
    this.prepareFlip_();
    node.collapsed = !node.collapsed;
    this.requestUpdate();
    this.browserProxy_.handler.setFolderCollapsed(id, node.collapsed);
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
  // Delete closes a tab, and F2 renames a tab or folder.
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
    const collapsed = node.collapsed;
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
        target.closest('compass-sidebar-node')?.startRename(id);
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

  // Right-clicking an entry (or the context menu key on one) opens its menu,
  // a native one the browser builds.
  protected onEntriesContextmenu_(e: MouseEvent) {
    const target = e.target as HTMLElement;
    const row = target.closest<HTMLElement>('[data-node-id]');
    if (!row || target instanceof HTMLInputElement) {
      return;
    }
    e.preventDefault();
    this.hideHoverCard_();
    // From the keyboard, the event's point needn't be on the row: open the
    // menu under it.
    const rect = row.getBoundingClientRect();
    const onRow = e.clientX >= rect.left && e.clientX <= rect.right &&
        e.clientY >= rect.top && e.clientY <= rect.bottom;
    this.browserProxy_.handler.showContextMenu(
        row.dataset['nodeId']!, Math.round(onRow ? e.clientX : rect.left),
        Math.round(onRow ? e.clientY : rect.bottom));
  }

  // The page's own menu (Back, Reload, Inspect) doesn't belong in the
  // sidebar. Text fields keep theirs.
  private onDocumentContextmenu_ = (e: MouseEvent) => {
    if (!(e.composedPath()[0] instanceof HTMLInputElement)) {
      e.preventDefault();
    }
  };

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

  private motionAllowed_(): boolean {
    return !window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  }

  private getStage_(): HTMLElement {
    return this.shadowRoot.querySelector<HTMLElement>('#stage')!;
  }

  private getSpacePane_(): HTMLElement {
    return this.shadowRoot.querySelector<HTMLElement>('#spacePane')!;
  }

  // The preview of the space |direction| (1 or -1) away, if any.
  private getPreview_(direction: number): HTMLElement|null {
    return direction ? this.shadowRoot.querySelector<HTMLElement>(
                           `compass-sidebar-preview[side="${
                               direction > 0 ? 'next' : 'previous'}"]`) :
                       null;
  }

  // Shows the neighbors' trees as far as they're known. Not mid-slide: the
  // preview sliding in must stay until the live pane takes its place.
  private updatePreviews_() {
    if (this.slide_) {
      return;
    }
    const previews: Preview[] = [];
    for (const [direction, side] of [[-1, 'previous'], [1, 'next']] as const) {
      const space = this.getNeighborSpace_(direction);
      if (space) {
        previews.push(
            {space, side, tree: this.spaceTrees_.get(space.id) ?? null});
      }
    }
    this.previews_ = previews;
  }

  // Fetches the other spaces' trees again, for entries that changed there.
  // The neighbors show theirs; a click further away shows its own.
  private refreshPreviews_() {
    for (const space of this.spaces_) {
      if (this.isActiveSpace_(space)) {
        continue;
      }
      this.browserProxy_.handler.getSpaceTree(space.id).then(({tree}) => {
        if (tree) {
          this.spaceTrees_.set(space.id, tree);
        } else {
          this.spaceTrees_.delete(space.id);
        }
        this.updatePreviews_();
      });
    }
    this.updatePreviews_();
  }

  // Back to where the swipe found them.
  private clearSwipeStyles_() {
    for (const element of [
           this.getSpacePane_(),
           ...this.shadowRoot.querySelectorAll<HTMLElement>(
               'compass-sidebar-preview'),
         ]) {
      element.style.removeProperty('transform');
      element.style.removeProperty('opacity');
    }
  }

  // A copy of |source| to animate in its place, appended to |container|. It
  // lives in a shadow root of its own, with the app's styles, so the app's
  // queries (and tests') don't find its rows. Returns its host and the copy.
  private makeGhost_(source: HTMLElement, container: HTMLElement):
      {host: HTMLElement, copy: HTMLElement} {
    const host = document.createElement('div');
    host.inert = true;
    const root = host.attachShadow({mode: 'open'});
    root.adoptedStyleSheets = this.shadowRoot.adoptedStyleSheets;
    for (const style of this.shadowRoot.querySelectorAll('style')) {
      root.append(style.cloneNode(true));
    }
    const copy = source.cloneNode(true) as HTMLElement;
    // Copies of entries would render again, empty: keep what they show, in
    // plain elements.
    for (const node of copy.querySelectorAll('compass-sidebar-node')) {
      const element = document.createElement('div');
      element.append(...node.childNodes);
      node.replaceWith(element);
    }
    root.append(copy);
    container.append(host);
    return {host, copy};
  }

  // Call before a change to the entries: once it's rendered, those that
  // moved glide from where they were (FLIP), new ones fade in and gone ones
  // fade out. Changes within a frame animate together.
  private prepareFlip_() {
    if (this.flipFirst_ || this.slide_ || this.swipe_ ||
        !this.motionAllowed_()) {
      return;
    }
    const first = new Map<string, FlipFirst>();
    for (const [key, element] of this.getFlipElements_()) {
      first.set(key, {rect: element.getBoundingClientRect(), element});
    }
    this.flipFirst_ = first;
    // The entries render in microtasks, before the frame.
    requestAnimationFrame(() => this.playFlip_());
  }

  // Tiles and rows by node ID, and the sections' other parts by `data-flip`.
  private getFlipElements_(): Map<string, HTMLElement> {
    const elements = new Map<string, HTMLElement>();
    for (const element of this.shadowRoot.querySelectorAll<HTMLElement>(
             '#favorites .tile, #spacePane .row, [data-flip]')) {
      const key = element.dataset['flip'] ?? element.dataset['nodeId'];
      if (key) {
        elements.set(key, element);
      }
    }
    return elements;
  }

  private playFlip_() {
    const first = this.flipFirst_;
    this.flipFirst_ = null;
    if (!first) {
      return;
    }
    // Moving again mid-glide starts from where it got to: |first| was
    // measured with the transforms.
    for (const animation of this.flipAnimations_) {
      animation.cancel();
    }
    this.flipAnimations_ = [];
    const last = this.getFlipElements_();
    for (const [key, element] of last) {
      const from = first.get(key)?.rect;
      const to = element.getBoundingClientRect();
      if (!from) {
        this.flipAnimations_.push(element.animate(
            [
              {opacity: 0, transform: 'translateY(-4px)'},
              {opacity: 1, transform: 'none'},
            ],
            {duration: FADE_MS, easing: EASE_OUT}));
        continue;
      }
      if (to.width === 0 || from.width === 0) {
        continue;
      }
      if (Math.abs(from.width - to.width) > to.width / 2 ||
          Math.abs(from.height - to.height) > to.height / 2) {
        // A row that became a tile, or the other way: it grows out of where
        // it was.
        const dx = from.left + from.width / 2 - (to.left + to.width / 2);
        const dy = from.top + from.height / 2 - (to.top + to.height / 2);
        this.flipAnimations_.push(element.animate(
            [
              {opacity: 0, transform: `translate(${dx}px, ${dy}px) scale(0.6)`},
              {opacity: 1, transform: 'none'},
            ],
            {duration: FLIP_MS, easing: EASE_OUT}));
        continue;
      }
      const dx = from.left - to.left;
      const dy = from.top - to.top;
      if (Math.abs(dx) < 0.5 && Math.abs(dy) < 0.5) {
        continue;
      }
      this.flipAnimations_.push(element.animate(
          [{transform: `translate(${dx}px, ${dy}px)`}, {transform: 'none'}],
          {duration: FLIP_MS, easing: EASE_OUT}));
    }
    for (const [key, {rect, element}] of first) {
      if (!last.has(key) && rect.width > 0) {
        this.fadeOut_(element, rect);
      }
    }
  }

  // A copy of |element|, gone, fades where it was, at |rect|.
  private fadeOut_(element: HTMLElement, rect: DOMRect) {
    const origin = this.getBoundingClientRect();
    const {host} = this.makeGhost_(
        element, this.shadowRoot.querySelector<HTMLElement>('#ghosts')!);
    Object.assign(host.style, {
      display: 'block',
      height: `${rect.height}px`,
      left: `${rect.left - origin.left}px`,
      padding: '0',
      position: 'absolute',
      top: `${rect.top - origin.top}px`,
      width: `${rect.width}px`,
    });
    host.animate([{opacity: 1}, {opacity: 0, transform: 'scale(0.96)'}],
                 {duration: FADE_MS, easing: EASE_OUT})
        .finished.then(() => host.remove(), () => host.remove());
  }

  // Two fingers across the sidebar pull in the next or the previous space.
  // Vertical scrolling and pinch zoom (ctrlKey) go on as usual.
  private onWheel_ = (e: WheelEvent) => {
    if (e.ctrlKey) {
      return;
    }
    this.clearWheelQuietTimer_();
    this.wheelQuietTimer_ =
        setTimeout(() => this.onWheelQuiet_(), WHEEL_QUIET_MS);
    if (!this.wheelAxis_) {
      if (Math.abs(e.deltaX) <= Math.abs(e.deltaY)) {
        this.wheelAxis_ = 'y';
      } else if (
          this.slide_ || this.menuSpaceId_ || this.archiveOpen_ ||
          this.draggingId_) {
        this.wheelAxis_ = 'done';
      } else {
        this.wheelAxis_ = 'x';
        this.startSwipe_();
      }
    }
    if (this.wheelAxis_ === 'y') {
      return;
    }
    e.preventDefault();
    if (this.wheelAxis_ === 'x' && this.swipe_) {
      const scale = e.deltaMode === WheelEvent.DOM_DELTA_LINE ? 16 : 1;
      this.swipe_.offset -= e.deltaX * scale;
      this.updateSwipe_();
    }
  };

  private clearWheelQuietTimer_() {
    if (this.wheelQuietTimer_ !== null) {
      clearTimeout(this.wheelQuietTimer_);
      this.wheelQuietTimer_ = null;
    }
  }

  // The gesture ended: a swipe past SWIPE_SETTLE switches, one short of it
  // goes back.
  private onWheelQuiet_() {
    this.wheelQuietTimer_ = null;
    this.wheelAxis_ = null;
    const swipe = this.swipe_;
    if (swipe) {
      this.endSwipe_(
          !!swipe.target &&
          Math.abs(this.getSwipeShift_(swipe)) >=
              SWIPE_SETTLE * this.getStage_().clientWidth);
    }
  }

  // While the entries slide, the lists don't scroll: their scrollbars would
  // flash.
  private setSliding_(sliding: boolean) {
    this.getStage_().toggleAttribute('sliding', sliding);
  }

  private startSwipe_() {
    this.hideHoverCard_();
    this.setSliding_(true);
    for (const animation of this.settleAnimations_) {
      animation.cancel();
    }
    this.settleAnimations_ = [];
    this.swipe_ = {offset: 0, direction: 0, target: null};
    // Usually there already; this catches up on what changed there.
    this.refreshPreviews_();
  }

  // The space |direction| (1 or -1) away from the window's, if any.
  private getNeighborSpace_(direction: number): Space|null {
    const index = this.spaces_.findIndex(space => this.isActiveSpace_(space));
    return direction && index !== -1 ?
        this.spaces_[index + direction] ?? null :
        null;
  }

  // How far the entries moved: with the fingers, up to the sidebar's width,
  // or only a little past the first and the last space.
  private getSwipeShift_(swipe: Swipe): number {
    if (!swipe.target) {
      return Math.sign(swipe.offset) *
          Math.min(Math.abs(swipe.offset) * SWIPE_RESISTANCE,
                   SWIPE_RESISTANCE_MAX_PX);
    }
    const width = this.getStage_().clientWidth;
    return Math.max(-width, Math.min(width, swipe.offset));
  }

  private updateSwipe_() {
    const swipe = this.swipe_!;
    const direction = Math.sign(-swipe.offset);
    if (direction !== swipe.direction) {
      swipe.direction = direction;
      swipe.target = this.getNeighborSpace_(direction);
      this.clearSwipeStyles_();
    }
    if (swipe.target &&
        Math.abs(swipe.offset) >=
            SWIPE_SWITCH * this.getStage_().clientWidth) {
      // The rest of the gesture is the switch's.
      this.wheelAxis_ = 'done';
      this.endSwipe_(/*commit=*/ true);
      return;
    }
    if (this.swipeFrame_ === null) {
      this.swipeFrame_ = requestAnimationFrame(() => {
        this.swipeFrame_ = null;
        this.applySwipe_();
      });
    }
  }

  private applySwipe_() {
    const swipe = this.swipe_;
    if (!swipe) {
      return;
    }
    const shift = this.getSwipeShift_(swipe);
    const width = this.getStage_().clientWidth;
    const progress = swipe.target ? Math.abs(shift) / width : 0;
    const pane = this.getSpacePane_();
    pane.style.transform = `translateX(${shift}px)`;
    pane.style.opacity = `${1 - progress / 2}`;
    const preview = this.getPreview_(swipe.direction);
    if (preview) {
      preview.style.transform =
          `translateX(${shift + swipe.direction * width}px)`;
      preview.style.opacity = `${0.5 + progress / 2}`;
    }
    // The tint follows.
    this.browserProxy_.handler.previewSpace(swipe.target?.id ?? null, progress);
  }

  // Lets go of the swipe: switches to the space it pulled in, the rest of the
  // way, or goes back.
  private endSwipe_(commit: boolean) {
    const swipe = this.swipe_;
    if (!swipe) {
      return;
    }
    this.swipe_ = null;
    if (this.swipeFrame_ !== null) {
      cancelAnimationFrame(this.swipeFrame_);
      this.swipeFrame_ = null;
    }
    const width = this.getStage_().clientWidth;
    const shift = this.getSwipeShift_(swipe);
    const progress = swipe.target ? Math.abs(shift) / width : 0;
    const direction = swipe.direction;
    const pane = this.getSpacePane_();
    const preview = swipe.target ? this.getPreview_(direction) : null;
    // The animations take over from where the frames left them.
    this.clearSwipeStyles_();
    const paneFrom = {
      opacity: 1 - progress / 2,
      transform: `translateX(${shift}px)`,
    };
    const previewFrom = {
      opacity: 0.5 + progress / 2,
      transform: `translateX(${shift + direction * width}px)`,
    };
    const options: KeyframeAnimationOptions = {
      duration: this.motionAllowed_() ? SLIDE_MS : 0,
      easing: EASE_OUT,
      fill: 'forwards',
    };
    if (commit && swipe.target) {
      this.slideTo_(swipe.target, direction, preview, shift);
      return;
    }
    this.browserProxy_.handler.previewSpace(null, 0);
    this.settleAnimations_ = [pane.animate(
        [paneFrom, {opacity: 1, transform: 'none'}],
        {...options, fill: 'none'})];
    if (!preview) {
      this.settleAnimations_[0]!.finished.then(() => {
        if (!this.swipe_ && !this.slide_) {
          this.setSliding_(false);
        }
      }, () => {});
      return;
    }
    const back = preview.animate(
        [
          previewFrom,
          {opacity: 0.5, transform: `translateX(${direction * width}px)`},
        ],
        options);
    this.settleAnimations_.push(back);
    back.finished.then(() => {
      if (!this.swipe_ && !this.slide_) {
        // Off to the side again.
        back.cancel();
        this.setSliding_(false);
      }
    }, () => {});
  }

  // Switches to |target|: slides its preview (null without one) in from
  // |shift| pixels along, the window's entries out, and the tint along,
  // then asks the browser. The live pane takes the preview's place once the
  // new tree is there (playSlide_()).
  private slideTo_(
      target: Space, direction: number, preview: HTMLElement|null,
      shift: number) {
    const width = this.getStage_().clientWidth;
    const progress = Math.abs(shift) / width;
    const options: KeyframeAnimationOptions = {
      duration: this.motionAllowed_() ? SLIDE_MS : 0,
      easing: EASE_OUT,
      fill: 'forwards',
    };
    // The old space leaves as a frozen copy: the browser switches mid-slide
    // and the live pane renders the new space's entries as they arrive,
    // which flashed in the half sliding out. The live pane stays out of sight
    // until the new space's preview has slid in, and takes its place then.
    const pane = this.getSpacePane_();
    const {host, copy} = this.makeGhost_(pane, this.getStage_());
    Object.assign(host.style, {
      height: `${pane.offsetHeight}px`,
      left: '0',
      padding: '0',
      position: 'absolute',
      top: '0',
      width: '100%',
    });
    // The swipe moved the live pane; the copy moves by its animation only.
    copy.style.removeProperty('transform');
    copy.style.removeProperty('opacity');
    const sections = copy.querySelector<HTMLElement>('#sections')!;
    sections.style.overflow = 'hidden';
    sections.scrollTop = this.getSections_().scrollTop;
    const animations = [
      host.animate(
          [
            {opacity: 1 - progress / 2, transform: `translateX(${shift}px)`},
            {opacity: 0.5, transform: `translateX(${-direction * width}px)`},
          ],
          options),
      pane.animate([{opacity: 0}, {opacity: 0}], options),
    ];
    if (preview) {
      animations.push(preview.animate(
          [
            {
              opacity: 0.5 + progress / 2,
              transform: `translateX(${shift + direction * width}px)`,
            },
            {opacity: 1, transform: 'none'},
          ],
          options));
    }
    this.endSlide_();
    // After endSlide_(), which removes the last slide's copy.
    this.slideGhost_ = host;
    this.setSliding_(true);
    const slide: Slide = {
      direction,
      fromPreview: true,
      switched: false,
      hasTree: false,
      animations,
    };
    this.slide_ = slide;
    this.slideTimer_ = setTimeout(() => this.endSlide_(), SWITCH_TIMEOUT_MS);
    // The tint goes with the slide, frame by frame, until the browser has
    // switched and fades it itself.
    const tint = () => {
      if (this.slide_ !== slide || slide.switched) {
        return;
      }
      const timing = animations[0]!.effect?.getComputedTiming();
      this.browserProxy_.handler.previewSpace(
          target.id, progress + (1 - progress) * (timing?.progress ?? 1));
      if (animations[0]!.playState === 'running') {
        requestAnimationFrame(tint);
      }
    };
    requestAnimationFrame(tint);
    let switched = false;
    const doSwitch = () => {
      if (!switched) {
        switched = true;
        // Another profile's swaps the window, which keeps this page: the
        // new window's tree comes as within a profile.
        this.browserProxy_.handler.switchSpace(target.id);
      }
    };
    // Switching keeps the browser busy for a moment: once the slide has
    // started. A hidden page gets no frames; it switches anyway.
    requestAnimationFrame(() => setTimeout(doSwitch));
    setTimeout(doSwitch, 50);
  }

  private onSetSpaces_(
      spaces: Space[], activeId: string, profiles: ProfileOption[]) {
    const previousId = this.activeSpaceId_;
    if (previousId && activeId !== previousId) {
      this.onActiveSpaceChanged_(previousId, activeId, spaces);
    }
    // Reordered: the buttons glide to their places.
    const ids = (list: Space[]) => list.map(space => space.id);
    if (ids(spaces).join() !== ids(this.spaces_).join() &&
        ids(spaces).sort().join() === ids(this.spaces_).sort().join()) {
      this.prepareFlip_();
    }
    this.spaces_ = spaces;
    this.profiles_ = profiles;
    this.activeSpaceId_ = activeId;
    this.refreshPreviews_();
  }

  // The window switched spaces. Unless a swipe slid the new space in
  // already, it slides in when its tree arrives: from the right if it comes
  // after the old one, else from the left.
  private onActiveSpaceChanged_(
      previousId: string, activeId: string, spaces: Space[]) {
    // The space left is a neighbor now, or soon: keep what it showed.
    if (this.favorites_ && this.pinned_ && this.today_) {
      this.spaceTrees_.set(previousId, {
        favorites: this.favorites_,
        pinned: this.pinned_,
        today: this.today_,
        // What it shows when the window comes back to it.
        activeId: this.activeId_,
      });
    }
    if (this.slide_?.fromPreview && !this.slide_.hasTree) {
      this.slide_.switched = true;
      return;
    }
    if (this.swipe_) {
      // Ctrl+1–9 mid-swipe.
      this.swipe_ = null;
      this.wheelAxis_ = 'done';
      this.clearSwipeStyles_();
    }
    this.endSlide_();
    this.setSliding_(true);
    const oldIndex = this.spaces_.findIndex(space => space.id === previousId);
    const newIndex = spaces.findIndex(space => space.id === activeId);
    this.slide_ = {
      direction: newIndex >= oldIndex ? 1 : -1,
      fromPreview: false,
      switched: true,
      hasTree: false,
      animations: [],
    };
    this.slideTimer_ = setTimeout(() => this.endSlide_(), SWITCH_TIMEOUT_MS);
  }

  // The new space's tree is about to replace the old one's.
  private playSlide_(slide: Slide) {
    slide.hasTree = true;
    if (slide.fromPreview) {
      // Its preview is where its entries go: they take its place once it's
      // there.
      Promise.all(slide.animations.map(animation => animation.finished))
          .then(() => {
            if (this.slide_ === slide) {
              this.endSlide_();
            }
          }, () => {});
      return;
    }
    if (!this.motionAllowed_()) {
      this.endSlide_();
      return;
    }
    const stage = this.getStage_();
    const pane = this.getSpacePane_();
    const {host, copy} = this.makeGhost_(pane, stage);
    Object.assign(host.style, {
      height: `${pane.offsetHeight}px`,
      left: '0',
      padding: '0',
      position: 'absolute',
      top: '0',
      width: '100%',
    });
    const sections = copy.querySelector<HTMLElement>('#sections')!;
    sections.style.overflow = 'hidden';
    sections.scrollTop = this.getSections_().scrollTop;
    this.slideGhost_ = host;
    const shift = slide.direction * stage.clientWidth;
    const options = {duration: SLIDE_MS, easing: EASE_OUT};
    slide.animations = [
      host.animate(
          [
            {opacity: 1, transform: 'none'},
            {opacity: 0.5, transform: `translateX(${-shift}px)`},
          ],
          {...options, fill: 'forwards'}),
      pane.animate(
          [
            {opacity: 0.5, transform: `translateX(${shift}px)`},
            {opacity: 1, transform: 'none'},
          ],
          options),
    ];
    Promise.all(slide.animations.map(animation => animation.finished))
        .then(() => {
          if (this.slide_ === slide) {
            this.endSlide_();
          }
        }, () => {});
  }

  // Puts the entries where the switch leaves them, at once: the new space's
  // in place. A swipe's switch that never came leaves the old space's.
  private endSlide_() {
    if (this.slideTimer_ !== null) {
      clearTimeout(this.slideTimer_);
      this.slideTimer_ = null;
    }
    const slide = this.slide_;
    this.slide_ = null;
    if (!slide) {
      return;
    }
    if (!this.swipe_) {
      this.setSliding_(false);
    }
    for (const animation of slide.animations) {
      animation.cancel();
    }
    this.slideGhost_?.remove();
    this.slideGhost_ = null;
    if (slide.fromPreview && !slide.hasTree) {
      this.browserProxy_.handler.previewSpace(null, 0);
    }
    // The previews slid in or out go back to the side, for the new
    // neighbors.
    this.updatePreviews_();
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
    if (drop && this.draggingId_ && drop.position.startsWith('split-')) {
      this.browserProxy_.handler.splitNode(
          this.draggingId_, drop.parent.id, drop.position === 'split-left');
    } else if (drop && this.draggingId_) {
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
    this.clearSpaceDrop_();
    this.draggingId_ = null;
    this.draggingSpaceId_ = null;
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
      // A tab on another tab's middle half: side by side, on the side it's
      // dropped on.
      if (dragged && this.canSplit_(dragged, node) && y > 1 / 4 &&
          y < 3 / 4) {
        const rtl = getComputedStyle(row).direction === 'rtl';
        const left = (e.clientX - rect.left < rect.width / 2) !== rtl;
        return {
          parent: node,
          index: 0,
          target: row,
          position: left ? 'split-left' : 'split-right',
        };
      }
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

  // Mirrors SpaceModel::CanSplit() for a tab entry dragged onto a tab entry,
  // outside the favorites.
  private canSplit_(dragged: Node, target: Node): boolean {
    return dragged !== target && dragged.type === NodeType.kTab &&
        target.type === NodeType.kTab &&
        this.parents_.get(dragged.id) !== this.favorites_ &&
        this.parents_.get(target.id) !== this.favorites_;
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
    return true;
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
    if (this.slide_ && !this.slide_.hasTree) {
      this.playSlide_(this.slide_);
    }
    this.refreshPreviews_();
    // Another profile's favorites (a switch to its space): its tiles fade in
    // and the space under them glides to its new place, rather than jumping.
    const ids = (root: Node|null) =>
        root?.children.map(node => node.id).join() ?? '';
    const stageTop = this.favorites_ && this.motionAllowed_() &&
            ids(this.favorites_) !== ids(tree.favorites) ?
        this.getStage_().getBoundingClientRect().top :
        null;
    this.nodes_.clear();
    this.parents_.clear();
    this.index_(tree.favorites, null);
    this.index_(tree.pinned, null);
    this.index_(tree.today, null);
    this.favorites_ = tree.favorites;
    this.pinned_ = tree.pinned;
    this.today_ = tree.today;
    this.activeId_ = tree.activeId;
    if (stageTop !== null) {
      this.updateComplete.then(() => this.glideFavorites_(stageTop));
    }
    // Lit renders it in microtasks, before the renderer paints or answers the
    // browser's next request.
    this.browserProxy_.handler.treeShown();
  }

  private glideFavorites_(oldStageTop: number) {
    const options = {duration: SLIDE_MS, easing: EASE_OUT};
    const stage = this.getStage_();
    const shift = oldStageTop - stage.getBoundingClientRect().top;
    if (Math.abs(shift) >= 0.5) {
      stage.animate(
          [{transform: `translateY(${shift}px)`}, {transform: 'none'}],
          options);
    }
    this.shadowRoot.querySelector<HTMLElement>('#favorites')
        ?.animate([{opacity: 0}, {opacity: 1}], options);
  }

  private onNodeAdded_(parentId: string, index: number, node: Node) {
    const parent = this.nodes_.get(parentId);
    if (!parent || this.nodes_.has(node.id)) {
      return;
    }
    this.prepareFlip_();
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
    this.prepareFlip_();
    oldParent.children.splice(oldParent.children.indexOf(node), 1);
    newParent.children.splice(index, 0, node);
    this.parents_.set(id, newParent);
    this.requestUpdate();
  }

  private onNodeChanged_(
      id: string, title: string, customTitle: string, url: string,
      collapsed: boolean) {
    const node = this.nodes_.get(id);
    if (!node) {
      return;
    }
    if (node.collapsed !== collapsed) {
      // Another window collapsed or expanded it.
      this.prepareFlip_();
    }
    node.title = title;
    node.customTitle = customTitle;
    node.url = url;
    node.collapsed = collapsed;
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
    this.prepareFlip_();
    parent.children.splice(parent.children.indexOf(node), 1);
    this.unindex_(node);
    // Maybe to a neighbor (Move to Space).
    this.refreshPreviews_();
    this.requestUpdate();
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-sidebar-app': CompassSidebarAppElement;
  }
}

customElements.define(CompassSidebarAppElement.is, CompassSidebarAppElement);
