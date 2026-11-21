// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A board's editor, chrome://easel/<uuid> (docs/design/easels.md,
// "Editor"): tools, selection, moving and resizing, pan and zoom by mouse,
// trackpad and touch, paste and drop, undo and redo. Every edit saves at
// once; a drag saves once, when the pointer is released.

import {BoardView, clampZoom} from './board_view.js';
import {EaselBrowserProxy} from './browser_proxy.js';
import {button, el, readStorage, toast, writeStorage} from './dom.js';
import {EaselState, ObjectKind} from './easel.mojom-webui.js';
import type {EaselObject, LinkContent} from './easel.mojom-webui.js';
import type {Point, Rect} from './geometry.js';
import {boxAround, distance, intersects, rectFromPoints, rectOf, scalePoints, unionRects} from './geometry.js';
import type {Change} from './history.js';
import {History} from './history.js';
import {icon} from './icons.js';
import {asWebUrl, blankObject, byZ, cloneObject, colorOf, COLORS, DEFAULT_STROKE_WIDTH, FONTS, hasColor, MAX_OBJECTS, MAX_TEXT_CHARS, maxZ, minZ, newUuid, PALETTES, paletteOf, ShapeKind, TEXT_STYLES, urlOf} from './model.js';

type Tool = 'select'|'text'|'rectangle'|'circle'|'arrow'|'pen';

const TOOLS: Array<{tool: Tool, name: string, key: string}> = [
  {tool: 'select', name: 'Select', key: 'v'},
  {tool: 'text', name: 'Text', key: 't'},
  {tool: 'rectangle', name: 'Rectangle', key: 'r'},
  {tool: 'circle', name: 'Circle', key: 'o'},
  {tool: 'arrow', name: 'Arrow', key: 'a'},
  {tool: 'pen', name: 'Pen', key: 'p'},
];

// Copied objects, on the clipboard beside their text.
const CLIPBOARD_TYPE = 'application/x-compass-easel';

// A press that moves less than this (in screen points) is a click.
const CLICK_SLOP = 4;

// The longest side of an added image, in board points.
const IMAGE_SIZE = 480;

// One pointer gesture on the board.
type Gesture = {
  type: 'move',
  start: Point,
  before: Map<string, EaselObject>,
  moved: boolean,
  target: string,
  wasSelected: boolean,
  pointerType: string,
  cancelled: boolean,
}|{
  type: 'resize',
  handle: string,
  start: Point,
  box: Rect,
  before: Map<string, EaselObject>,
  moved: boolean,
}|{
  type: 'marquee',
  start: Point,
  current: Point,
  initial: Set<string>,
}|{
  type: 'pan',
  startClient: Point,
  startPan: Point,
  moved: boolean,
}|{
  type: 'draw',
  kind: ShapeKind,
  start: Point,
  current: Point,
}|{
  type: 'pen',
  points: number[],
};

interface Pinch {
  startDistance: number;
  startZoom: number;
  // The board point under the fingers' middle when they came down.
  anchor: Point;
}

interface Editing {
  uuid: string;
  isNew: boolean;
  before: EaselObject|null;
}

export class Editor {
  private readonly uuid_: string;
  private readonly proxy_ = EaselBrowserProxy.getInstance();
  private readonly root_: HTMLElement;
  private view_: BoardView|null = null;
  private objects_ = new Map<string, EaselObject>();
  private selection_ = new Set<string>();
  private history_ = new History();
  private tool_: Tool = 'select';
  private color_ = '';
  private title_ = '';
  private palette_ = 'paper';
  private gesture_: Gesture|null = null;
  private pinch_: Pinch|null = null;
  private pointers_ = new Map<number, Point>();
  private editing_: Editing|null = null;
  private spaceDown_ = false;
  // The last board point the pointer was at, where pastes go.
  private lastPoint_: Point|null = null;
  // Per object, the latest save sent: an older save's answer doesn't
  // overwrite a newer local edit.
  private saveSeq_ = new Map<string, number>();
  private lastSaveSeq_ = 0;
  private refreshTimer_ = 0;
  private viewTimer_ = 0;
  private loaded_ = false;
  // A shared easel this profile can only view (docs/design/sync.md, "Rooms:
  // clients"): nothing on the board changes here.
  private viewOnly_ = false;

  // Built once loaded.
  private titleInput_: HTMLInputElement|null = null;
  private paletteSelect_: HTMLSelectElement|null = null;
  private zoomLabel_: HTMLButtonElement|null = null;
  private toolButtons_ = new Map<Tool, HTMLButtonElement>();
  private colorButtons_: HTMLButtonElement[] = [];
  private undoButton_: HTMLButtonElement|null = null;
  private redoButton_: HTMLButtonElement|null = null;
  private arrangeBar_: HTMLElement|null = null;
  private textBar_: HTMLElement|null = null;
  private styleSelect_: HTMLSelectElement|null = null;
  private fontSelect_: HTMLSelectElement|null = null;
  private fillButton_: HTMLButtonElement|null = null;
  private fileInput_: HTMLInputElement|null = null;
  private menu_: HTMLElement|null = null;

  constructor(root: HTMLElement, uuid: string) {
    this.root_ = root;
    this.uuid_ = uuid;
    this.proxy_.callbackRouter.onEaselChanged.addListener(
        (changed: string) => {
          if (changed === '' || changed === this.uuid_) {
            this.scheduleRefresh_(80);
          }
        });
  }

  async start() {
    await this.refresh_();
  }

  // ---------------------------------------------------------------------
  // Loading and live updates.

  private scheduleRefresh_(delay: number) {
    clearTimeout(this.refreshTimer_);
    this.refreshTimer_ = setTimeout(() => {
      // Not under a gesture: it would move what's being dragged.
      if (this.gesture_ || this.pinch_) {
        this.scheduleRefresh_(200);
        return;
      }
      void this.refresh_();
    }, delay);
  }

  private async refresh_() {
    const {state, easel, objects} =
        await this.proxy_.handler.getEasel(this.uuid_);
    if (state !== EaselState.kOk || !easel) {
      this.showMissing_(state);
      return;
    }
    if (!this.loaded_) {
      this.build_();
      this.loaded_ = true;
    }
    this.viewOnly_ = easel.viewOnly;
    this.root_.toggleAttribute('view-only', this.viewOnly_);
    if (this.titleInput_) {
      this.titleInput_.readOnly = this.viewOnly_;
      this.titleInput_.title = this.viewOnly_ ? 'View only' : '';
    }
    if (this.paletteSelect_) {
      this.paletteSelect_.disabled = this.viewOnly_;
    }
    this.title_ = easel.title;
    if (this.titleInput_ && document.activeElement !== this.titleInput_) {
      this.titleInput_.value = easel.title;
    }
    this.updateDocumentTitle_();
    this.palette_ = paletteOf(easel.palette);
    if (this.paletteSelect_) {
      this.paletteSelect_.value = this.palette_;
    }
    this.view_!.setPalette(this.palette_);

    // What's under the user's hands stays as it is here.
    const busy = new Set<string>();
    if (this.editing_) {
      busy.add(this.editing_.uuid);
    }
    const incoming = new Map(objects.map(o => [o.uuid, o]));
    for (const uuid of [...this.objects_.keys()]) {
      if (!incoming.has(uuid) && !busy.has(uuid)) {
        this.objects_.delete(uuid);
        this.selection_.delete(uuid);
        this.view_!.remove(uuid);
      }
    }
    for (const object of objects) {
      if (busy.has(object.uuid)) {
        continue;
      }
      const local = this.objects_.get(object.uuid);
      if (local && local.base === object.base && object.base) {
        continue;
      }
      this.objects_.set(object.uuid, object);
      this.view_!.update(object);
    }
    this.view_!.reorder(this.sorted_());
    this.updateSelection_();
  }

  private showMissing_(state: EaselState) {
    this.loaded_ = false;
    document.title = 'Easel';
    const message = state === EaselState.kUnavailable ?
        'Easels aren\'t available in Incognito and Guest windows.' :
        'This Easel isn\'t on this device. It may have been deleted, or ' +
            'not synced here yet.';
    this.root_.replaceChildren(el('div', {className: 'missing'}, [
      el('p', {text: message}),
      el('a', {text: 'All Easels', attrs: {'href': '/'}}),
    ]));
    this.view_ = null;
  }

  private updateDocumentTitle_() {
    document.title = this.title_ || 'Untitled Easel';
  }

  private sorted_(): EaselObject[] {
    return [...this.objects_.values()].sort(byZ);
  }

  // ---------------------------------------------------------------------
  // The page.

  private build_() {
    const viewport = el('div', {
      className: 'viewport',
      attrs: {'tabindex': '0', 'aria-label': 'Easel'},
    });
    this.view_ = new BoardView(viewport);

    const header = this.buildHeader_();
    const toolbar = this.buildToolbar_();
    this.menu_ = el('div', {className: 'menu', attrs: {'role': 'menu'}});
    this.menu_.hidden = true;
    this.fileInput_ = el('input', {
      className: 'file-input',
      attrs: {'type': 'file', 'accept': 'image/*', 'multiple': ''},
    });
    this.fileInput_.addEventListener('change', () => {
      const files = [...(this.fileInput_?.files || [])];
      void this.addFiles_(files, this.view_!.center());
      this.fileInput_!.value = '';
    });

    this.root_.replaceChildren(
        el('div', {className: 'editor'},
           [header, viewport, toolbar, this.menu_, this.fileInput_]));

    this.restoreView_();
    this.listen_(viewport);
    this.updateToolbar_();
  }

  private buildHeader_(): HTMLElement {
    const back = el('a', {
      className: 'back',
      text: 'Easels',
      attrs: {'href': '/', 'title': 'All Easels'},
    });
    const title = el('input', {
      className: 'title',
      attrs: {
        'type': 'text',
        'aria-label': 'Title',
        'placeholder': 'Untitled Easel',
        'maxlength': '200',
        'spellcheck': 'false',
      },
    });
    title.addEventListener('input', () => {
      this.title_ = title.value;
      this.updateDocumentTitle_();
    });
    title.addEventListener('change', () => {
      this.title_ = title.value.trim();
      this.proxy_.handler.renameEasel(this.uuid_, this.title_);
      this.updateDocumentTitle_();
    });
    title.addEventListener('keydown', (e: KeyboardEvent) => {
      if (e.key === 'Enter' || e.key === 'Escape') {
        title.blur();
      }
    });
    this.titleInput_ = title;

    const palette = el('select', {
      className: 'palette',
      attrs: {'aria-label': 'Background', 'title': 'Background'},
    });
    for (const p of PALETTES) {
      palette.appendChild(el('option', {text: p.name, attrs: {'value': p.value}}));
    }
    palette.addEventListener('change', () => {
      this.palette_ = palette.value;
      this.view_?.setPalette(this.palette_);
      this.proxy_.handler.setPalette(this.uuid_, this.palette_);
    });
    this.paletteSelect_ = palette;

    this.zoomLabel_ = button('Zoom to 100%', 'zoom', () => {
      this.zoomAround_(this.viewportCenterClient_(), 1);
    });
    const fit = button('Fit (Shift+1)', 'icon-button', () => this.fit_(),
                       icon('fit'));

    return el('header', {className: 'topbar'}, [
      back,
      title,
      el('div', {className: 'spacer'}),
      palette,
      this.zoomLabel_,
      fit,
    ]);
  }

  private buildToolbar_(): HTMLElement {
    const tools = el('div', {className: 'group', attrs: {'role': 'toolbar'}});
    for (const {tool, name, key} of TOOLS) {
      const b = button(`${name} (${key.toUpperCase()})`, 'icon-button',
                       () => this.setTool_(tool), icon(tool));
      this.toolButtons_.set(tool, b);
      tools.appendChild(b);
    }
    tools.appendChild(button('Image (I)', 'icon-button',
                             () => this.fileInput_?.click(), icon('image')));

    const colors = el('div', {className: 'group colors'});
    for (const color of COLORS) {
      const swatch = el('span', {className: 'swatch'});
      if (color.value) {
        swatch.style.background = color.value;
      }
      const b = button(color.name, 'color-button',
                       () => this.setColor_(color.value), swatch);
      b.dataset['color'] = color.value;
      this.colorButtons_.push(b);
      colors.appendChild(b);
    }

    // Shown for a selection.
    this.styleSelect_ = el('select', {attrs: {'aria-label': 'Text style'}});
    for (const s of TEXT_STYLES) {
      this.styleSelect_.appendChild(
          el('option', {text: s.name, attrs: {'value': s.value}}));
    }
    this.styleSelect_.addEventListener(
        'change', () => this.setTextField_('style', this.styleSelect_!.value));
    this.fontSelect_ = el('select', {attrs: {'aria-label': 'Font'}});
    for (const f of FONTS) {
      this.fontSelect_.appendChild(
          el('option', {text: f.name, attrs: {'value': f.value}}));
    }
    this.fontSelect_.addEventListener(
        'change', () => this.setTextField_('font', this.fontSelect_!.value));
    this.textBar_ = el('div', {className: 'group'},
                       [this.styleSelect_, this.fontSelect_]);
    this.fillButton_ =
        button('Fill', 'icon-button', () => this.toggleFill_(), icon('fill'));
    this.arrangeBar_ = el('div', {className: 'group'}, [
      this.fillButton_,
      button('Bring to front (])', 'icon-button', () => this.arrange_(true),
             icon('front')),
      button('Send to back ([)', 'icon-button', () => this.arrange_(false),
             icon('back')),
      button('Duplicate (⌘D)', 'icon-button', () => this.duplicate_(),
             icon('duplicate')),
      button('Delete (⌫)', 'icon-button', () => this.deleteSelection_(),
             icon('delete')),
    ]);

    this.undoButton_ =
        button('Undo (⌘Z)', 'icon-button', () => this.undo_(), icon('undo'));
    this.redoButton_ =
        button('Redo (⇧⌘Z)', 'icon-button', () => this.redo_(), icon('redo'));
    const history =
        el('div', {className: 'group'}, [this.undoButton_, this.redoButton_]);

    return el('div', {className: 'toolbar'}, [
      tools,
      colors,
      this.textBar_,
      this.arrangeBar_,
      history,
    ]);
  }

  private updateToolbar_() {
    for (const [tool, b] of this.toolButtons_) {
      b.classList.toggle('active', tool === this.tool_);
      b.setAttribute('aria-pressed', `${tool === this.tool_}`);
    }
    const selected = this.selectedObjects_();
    const colored = selected.filter(hasColor);
    const shown = colored.length ? colorOf(colored[0]!) : this.color_;
    for (const b of this.colorButtons_) {
      b.classList.toggle('active', (b.dataset['color'] || '') === shown);
    }
    const texts = selected.filter(o => o.kind === ObjectKind.kText && o.text);
    if (this.textBar_) {
      this.textBar_.hidden = texts.length === 0;
    }
    const firstText = texts[0]?.text;
    if (firstText && this.styleSelect_ && this.fontSelect_) {
      this.styleSelect_.value =
          TEXT_STYLES.some(s => s.value === firstText.style) ?
          firstText.style :
          '';
      this.fontSelect_.value =
          FONTS.some(f => f.value === firstText.font) ? firstText.font : '';
    }
    if (this.arrangeBar_) {
      this.arrangeBar_.hidden = selected.length === 0;
    }
    if (this.fillButton_) {
      const fillable = selected.filter(isFillable);
      this.fillButton_.hidden = fillable.length === 0;
      this.fillButton_.classList.toggle(
          'active', fillable.length > 0 && fillable.every(o => o.shape!.filled));
    }
    if (this.undoButton_ && this.redoButton_) {
      this.undoButton_.disabled = !this.history_.canUndo();
      this.redoButton_.disabled = !this.history_.canRedo();
    }
    if (this.zoomLabel_ && this.view_) {
      this.zoomLabel_.textContent = `${Math.round(this.view_.zoom * 100)}%`;
    }
  }

  private setTool_(tool: Tool) {
    this.finishEditing_();
    this.tool_ = tool;
    if (tool !== 'select') {
      this.selection_.clear();
      this.updateSelection_();
    }
    this.view_?.viewport.setAttribute('data-tool', tool);
    this.updateToolbar_();
  }

  // ---------------------------------------------------------------------
  // The view: pan and zoom.

  private viewKey_(): string {
    return `compass-easel-view:${this.uuid_}`;
  }

  private restoreView_() {
    const saved = readStorage(this.viewKey_());
    if (saved) {
      try {
        const {x, y, zoom} = JSON.parse(saved);
        if ([x, y, zoom].every(v => typeof v === 'number' && isFinite(v))) {
          this.view_!.setView(x, y, zoom);
          return;
        }
      } catch {
        // Fit instead.
      }
    }
    // Laid out first, so the viewport has its size.
    requestAnimationFrame(() => this.fit_());
  }

  private setView_(x: number, y: number, zoom: number) {
    if (!this.view_) {
      return;
    }
    this.view_.setView(x, y, zoom);
    this.updateSelection_();
    if (this.zoomLabel_) {
      this.zoomLabel_.textContent = `${Math.round(this.view_.zoom * 100)}%`;
    }
    clearTimeout(this.viewTimer_);
    this.viewTimer_ = setTimeout(() => {
      if (this.view_) {
        writeStorage(this.viewKey_(), JSON.stringify({
          x: this.view_.panX,
          y: this.view_.panY,
          zoom: this.view_.zoom,
        }));
      }
    }, 300);
  }

  private viewportCenterClient_(): Point {
    const r = this.view_!.viewport.getBoundingClientRect();
    return {x: r.left + r.width / 2, y: r.top + r.height / 2};
  }

  // Zooms to |zoom| keeping the board point under |client| in place.
  private zoomAround_(client: Point, zoom: number) {
    const view = this.view_!;
    const anchor = view.toBoard(client.x, client.y);
    const r = view.viewport.getBoundingClientRect();
    const z = clampZoom(zoom);
    this.setView_(client.x - r.left - anchor.x * z,
                  client.y - r.top - anchor.y * z, z);
  }

  // Shows everything (docs/design/easels.md, "Fit").
  private fit_() {
    const view = this.view_;
    if (!view) {
      return;
    }
    const r = view.viewport.getBoundingClientRect();
    const box = unionRects([...this.objects_.values()].map(rectOf));
    if (!box || r.width === 0) {
      this.setView_(r.width / 2, r.height / 2, 1);
      return;
    }
    const margin = 48;
    const zoom = clampZoom(Math.min(
        (r.width - 2 * margin) / Math.max(box.width, 1),
        (r.height - 2 * margin - 64) / Math.max(box.height, 1), 1.5));
    this.setView_(
        r.width / 2 - (box.x + box.width / 2) * zoom,
        (r.height - 64) / 2 - (box.y + box.height / 2) * zoom, zoom);
  }

  // ---------------------------------------------------------------------
  // Input.

  private listen_(viewport: HTMLElement) {
    viewport.addEventListener('pointerdown', e => this.onPointerDown_(e));
    viewport.addEventListener('pointermove', e => this.onPointerMove_(e));
    viewport.addEventListener('pointerup', e => this.onPointerUp_(e));
    viewport.addEventListener('pointercancel', e => this.onPointerUp_(e, true));
    viewport.addEventListener('dblclick', e => this.onDoubleClick_(e));
    viewport.addEventListener('wheel', e => this.onWheel_(e), {passive: false});
    viewport.addEventListener('contextmenu', e => this.onContextMenu_(e));
    viewport.addEventListener('dragover', e => {
      e.preventDefault();
      if (e.dataTransfer) {
        e.dataTransfer.dropEffect = 'copy';
      }
    });
    viewport.addEventListener('drop', e => this.onDrop_(e));
    document.addEventListener('keydown', e => this.onKeyDown_(e));
    document.addEventListener('keyup', e => {
      if (e.key === ' ') {
        this.spaceDown_ = false;
        this.view_?.viewport.classList.remove('panning');
      }
    });
    document.addEventListener('paste', e => this.onPaste_(e));
    document.addEventListener('copy', e => this.onCopy_(e, false));
    document.addEventListener('cut', e => this.onCopy_(e, true));
    document.addEventListener('pointerdown', e => {
      if (this.menu_ && !this.menu_.hidden &&
          !(e.target instanceof Node && this.menu_.contains(e.target))) {
        this.hideMenu_();
      }
    }, true);
    window.addEventListener('resize', () => this.updateSelection_());
  }

  private onPointerDown_(e: PointerEvent) {
    if (this.viewOnly_) {
      return;
    }
    const view = this.view_!;
    if (this.editing_) {
      const body = view.textBody(this.editing_.uuid);
      if (body && e.target instanceof Node && body.contains(e.target)) {
        // Placing the caret in the text being edited.
        return;
      }
      this.finishEditing_();
    }
    if (e.pointerType === 'mouse' && e.button === 2) {
      // The context menu.
      return;
    }
    // No mousedown after this: it would move the focus (out of a text just
    // put in editing) and select page text.
    e.preventDefault();
    view.viewport.focus({preventScroll: true});
    view.viewport.setPointerCapture(e.pointerId);
    this.pointers_.set(e.pointerId, {x: e.clientX, y: e.clientY});

    if (this.pointers_.size === 2) {
      // A second finger: pinch and pan, whatever the first one started.
      this.cancelGesture_();
      this.startPinch_();
      return;
    }
    if (this.pointers_.size > 2 || this.pinch_) {
      return;
    }

    const point = view.toBoard(e.clientX, e.clientY);
    this.lastPoint_ = point;
    if (e.button === 1 || (e.button === 0 && this.spaceDown_)) {
      this.startPan_(e);
      return;
    }

    const target = view.uuidAt(e.target);
    const handle = e.target instanceof HTMLElement && e.target.dataset['handle'];
    switch (this.tool_) {
      case 'select':
        if (handle) {
          this.startResize_(handle, point);
        } else if (target) {
          this.startMove_(target, point, e);
        } else if (e.pointerType === 'touch') {
          this.selection_.clear();
          this.updateSelection_();
          this.startPan_(e);
        } else {
          if (!e.shiftKey) {
            this.selection_.clear();
          }
          this.gesture_ = {
            type: 'marquee',
            start: point,
            current: point,
            initial: new Set(this.selection_),
          };
          this.updateSelection_();
        }
        break;
      case 'text': {
        const object = target ? this.objects_.get(target) : undefined;
        if (object && object.kind === ObjectKind.kText) {
          this.select_([object.uuid]);
          this.startEditing_(object.uuid, false);
        } else {
          this.addText_(point);
        }
        break;
      }
      case 'rectangle':
      case 'circle':
      case 'arrow':
        this.gesture_ = {
          type: 'draw',
          kind: this.tool_ === 'rectangle' ? ShapeKind.RECTANGLE :
              this.tool_ === 'circle'      ? ShapeKind.CIRCLE :
                                             ShapeKind.ARROW,
          start: point,
          current: point,
        };
        break;
      case 'pen':
        this.gesture_ = {type: 'pen', points: [point.x, point.y]};
        this.drawPenDraft_();
        break;
    }
  }

  private startPan_(e: PointerEvent) {
    const view = this.view_!;
    this.gesture_ = {
      type: 'pan',
      startClient: {x: e.clientX, y: e.clientY},
      startPan: {x: view.panX, y: view.panY},
      moved: false,
    };
    view.viewport.classList.add('panning');
  }

  private startMove_(target: string, point: Point, e: PointerEvent) {
    const wasSelected = this.selection_.has(target);
    if (e.shiftKey) {
      if (wasSelected) {
        this.selection_.delete(target);
        this.updateSelection_();
        return;
      }
      this.selection_.add(target);
    } else if (!wasSelected) {
      this.selection_ = new Set([target]);
    }
    this.updateSelection_();
    const before = new Map<string, EaselObject>();
    for (const uuid of this.selection_) {
      const object = this.objects_.get(uuid);
      if (object) {
        before.set(uuid, cloneObject(object));
      }
    }
    this.gesture_ = {
      type: 'move',
      start: point,
      before,
      moved: false,
      target,
      wasSelected,
      pointerType: e.pointerType,
      cancelled: false,
    };
  }

  private startResize_(handle: string, point: Point) {
    const objects = this.selectedObjects_();
    const box = unionRects(objects.map(rectOf));
    if (!box) {
      return;
    }
    this.gesture_ = {
      type: 'resize',
      handle,
      start: point,
      box,
      before: new Map(objects.map(o => [o.uuid, cloneObject(o)])),
      moved: false,
    };
  }

  private startPinch_() {
    const [a, b] = [...this.pointers_.values()];
    if (!a || !b) {
      return;
    }
    const middle = {x: (a.x + b.x) / 2, y: (a.y + b.y) / 2};
    this.pinch_ = {
      startDistance: Math.max(distance(a, b), 1),
      startZoom: this.view_!.zoom,
      anchor: this.view_!.toBoard(middle.x, middle.y),
    };
  }

  private onPointerMove_(e: PointerEvent) {
    const view = this.view_;
    if (!view) {
      return;
    }
    if (this.pointers_.has(e.pointerId)) {
      this.pointers_.set(e.pointerId, {x: e.clientX, y: e.clientY});
    }
    const point = view.toBoard(e.clientX, e.clientY);
    if (e.pointerType !== 'touch') {
      this.lastPoint_ = point;
    }
    if (this.pinch_) {
      this.movePinch_();
      return;
    }
    const gesture = this.gesture_;
    if (!gesture || !this.pointers_.has(e.pointerId)) {
      return;
    }
    switch (gesture.type) {
      case 'move': {
        const dx = point.x - gesture.start.x;
        const dy = point.y - gesture.start.y;
        if (!gesture.moved &&
            Math.hypot(dx, dy) * view.zoom < CLICK_SLOP) {
          return;
        }
        gesture.moved = true;
        for (const [uuid, before] of gesture.before) {
          const object = this.objects_.get(uuid);
          if (!object) {
            continue;
          }
          object.x = before.x + dx;
          object.y = before.y + dy;
          view.place(object);
        }
        this.updateSelection_();
        break;
      }
      case 'resize':
        gesture.moved = true;
        this.resizeTo_(gesture, point, e.shiftKey);
        break;
      case 'marquee': {
        gesture.current = point;
        const area = rectFromPoints(gesture.start, point);
        const selection = new Set(gesture.initial);
        for (const object of this.objects_.values()) {
          if (intersects(area, rectOf(object))) {
            selection.add(object.uuid);
          }
        }
        this.selection_ = selection;
        this.updateSelection_();
        break;
      }
      case 'pan':
        gesture.moved = true;
        this.setView_(
            gesture.startPan.x + e.clientX - gesture.startClient.x,
            gesture.startPan.y + e.clientY - gesture.startClient.y,
            view.zoom);
        break;
      case 'draw':
        gesture.current = e.shiftKey ? constrain(gesture, point) : point;
        view.drawDraft({
          kind: gesture.kind,
          rect: rectFromPoints(gesture.start, gesture.current),
          points: [
            gesture.start.x,
            gesture.start.y,
            gesture.current.x,
            gesture.current.y,
          ],
          color: this.color_,
        });
        break;
      case 'pen': {
        // Every sample the pointer had since the last frame, for a smooth
        // line.
        const events = e.getCoalescedEvents();
        for (const sample of events.length ? events : [e]) {
          const p = view.toBoard(sample.clientX, sample.clientY);
          const n = gesture.points.length;
          const last = {
            x: gesture.points[n - 2] ?? p.x,
            y: gesture.points[n - 1] ?? p.y,
          };
          if (distance(last, p) * view.zoom >= 1) {
            gesture.points.push(p.x, p.y);
          }
        }
        this.drawPenDraft_();
        break;
      }
    }
  }

  private resizeTo_(
      gesture: Extract<Gesture, {type: 'resize'}>, point: Point,
      free: boolean) {
    const view = this.view_!;
    const box = gesture.box;
    const dx = point.x - gesture.start.x;
    const dy = point.y - gesture.start.y;
    let left = box.x;
    let top = box.y;
    let right = box.x + box.width;
    let bottom = box.y + box.height;
    const h = gesture.handle;
    if (h.includes('w')) {
      left = Math.min(left + dx, right - 4);
    }
    if (h.includes('e')) {
      right = Math.max(right + dx, left + 4);
    }
    if (h.includes('n')) {
      top = Math.min(top + dy, bottom - 4);
    }
    if (h.includes('s')) {
      bottom = Math.max(bottom + dy, top + 4);
    }
    // Images and cards keep their shape from a corner, unless Shift.
    const corner = h.length === 2;
    const keepRatio = corner && !free &&
        [...gesture.before.values()].some(
            o => o.kind === ObjectKind.kImage ||
                o.kind === ObjectKind.kCapture || gesture.before.size > 1);
    if (keepRatio && box.width > 0 && box.height > 0) {
      const scale = Math.max((right - left) / box.width,
                             (bottom - top) / box.height);
      const width = box.width * scale;
      const height = box.height * scale;
      if (h.includes('w')) {
        left = right - width;
      } else {
        right = left + width;
      }
      if (h.includes('n')) {
        top = bottom - height;
      } else {
        bottom = top + height;
      }
    }
    const sx = box.width > 0 ? (right - left) / box.width : 1;
    const sy = box.height > 0 ? (bottom - top) / box.height : 1;
    for (const [uuid, before] of gesture.before) {
      const object = this.objects_.get(uuid);
      if (!object) {
        continue;
      }
      object.x = left + (before.x - box.x) * sx;
      object.y = top + (before.y - box.y) * sy;
      object.width = Math.max(1, before.width * sx);
      object.height = Math.max(1, before.height * sy);
      if (object.shape && before.shape) {
        object.shape.points = scalePoints(
            before.shape.points, object.width / Math.max(before.width, 1),
            object.height / Math.max(before.height, 1));
        view.update(object);
      } else {
        view.place(object);
      }
    }
    this.updateSelection_();
  }

  private movePinch_() {
    const pinch = this.pinch_;
    const [a, b] = [...this.pointers_.values()];
    if (!pinch || !a || !b) {
      return;
    }
    const view = this.view_!;
    const r = view.viewport.getBoundingClientRect();
    const middle = {x: (a.x + b.x) / 2, y: (a.y + b.y) / 2};
    const zoom =
        clampZoom(pinch.startZoom * distance(a, b) / pinch.startDistance);
    this.setView_(middle.x - r.left - pinch.anchor.x * zoom,
                  middle.y - r.top - pinch.anchor.y * zoom, zoom);
  }

  private onPointerUp_(e: PointerEvent, cancelled = false) {
    const view = this.view_;
    if (!view || !this.pointers_.has(e.pointerId)) {
      return;
    }
    this.pointers_.delete(e.pointerId);
    if (this.pinch_) {
      if (this.pointers_.size < 2) {
        this.pinch_ = null;
        // The finger left on the board starts nothing until it lifts.
      }
      return;
    }
    const gesture = this.gesture_;
    this.gesture_ = null;
    view.viewport.classList.remove('panning');
    if (!gesture) {
      return;
    }
    if (cancelled) {
      this.restore_(gesture);
      return;
    }
    switch (gesture.type) {
      case 'move':
        if (gesture.moved) {
          this.commitFrom_(gesture.before);
        } else if (!gesture.cancelled && !e.shiftKey) {
          this.clicked_(gesture);
        }
        break;
      case 'resize':
        if (gesture.moved) {
          this.commitFrom_(gesture.before);
        }
        break;
      case 'marquee':
        this.updateSelection_();
        break;
      case 'pan':
        if (!gesture.moved && e.pointerType === 'touch') {
          this.selection_.clear();
          this.updateSelection_();
        }
        break;
      case 'draw':
        view.drawDraft(null);
        this.addShape_(gesture.kind, gesture.start, gesture.current);
        break;
      case 'pen':
        view.drawDraft(null);
        this.addStroke_(gesture.points);
        break;
    }
  }

  // A press on an object that didn't move it.
  private clicked_(gesture: Extract<Gesture, {type: 'move'}>) {
    const object = this.objects_.get(gesture.target);
    if (!object) {
      return;
    }
    this.selection_ = new Set([object.uuid]);
    this.updateSelection_();
    const url = urlOf(object);
    if (url) {
      // docs/design/easels.md: a click opens a link or capture.
      this.proxy_.handler.openUrl(url);
      return;
    }
    if (object.kind === ObjectKind.kText && gesture.wasSelected &&
        gesture.pointerType === 'touch') {
      // A tap on the selected text edits it, as a double click does.
      this.startEditing_(object.uuid, false);
    }
  }

  // Puts back what a cancelled gesture changed.
  private restore_(gesture: Gesture) {
    const view = this.view_!;
    if (gesture.type === 'move' || gesture.type === 'resize') {
      for (const [uuid, before] of gesture.before) {
        if (this.objects_.has(uuid)) {
          this.objects_.set(uuid, before);
          view.update(before);
        }
      }
    }
    view.drawDraft(null);
    this.updateSelection_();
  }

  private cancelGesture_() {
    const gesture = this.gesture_;
    this.gesture_ = null;
    if (gesture) {
      this.restore_(gesture);
    }
  }

  // The object at a click: its target is the viewport, which captured the
  // pointer.
  private uuidAtPoint_(e: MouseEvent): string|null {
    return this.view_!.uuidAt(document.elementFromPoint(e.clientX, e.clientY));
  }

  private onDoubleClick_(e: MouseEvent) {
    const uuid = this.uuidAtPoint_(e);
    const object = uuid ? this.objects_.get(uuid) : undefined;
    if (object && object.kind === ObjectKind.kText &&
        this.editing_?.uuid !== object.uuid) {
      this.select_([object.uuid]);
      this.startEditing_(object.uuid, false);
    } else if (!object && this.tool_ === 'select') {
      // A double click on the board writes there.
      this.addText_(this.view_!.toBoard(e.clientX, e.clientY));
    }
  }

  private onWheel_(e: WheelEvent) {
    if (!this.view_ || this.editing_) {
      return;
    }
    e.preventDefault();
    const scale = e.deltaMode === WheelEvent.DOM_DELTA_LINE ? 16 : 1;
    if (e.ctrlKey || e.metaKey) {
      // A trackpad's pinch comes as Ctrl + wheel.
      const factor = Math.exp(-e.deltaY * scale * 0.01);
      this.zoomAround_({x: e.clientX, y: e.clientY},
                       this.view_.zoom * factor);
      return;
    }
    let dx = e.deltaX * scale;
    let dy = e.deltaY * scale;
    if (e.shiftKey && dx === 0) {
      dx = dy;
      dy = 0;
    }
    this.setView_(this.view_.panX - dx, this.view_.panY - dy, this.view_.zoom);
  }

  private onKeyDown_(e: KeyboardEvent) {
    if (!this.view_ || this.viewOnly_) {
      return;
    }
    const target = e.target;
    if (target instanceof HTMLElement &&
        (target.isContentEditable || target instanceof HTMLInputElement ||
         target instanceof HTMLSelectElement ||
         target instanceof HTMLTextAreaElement)) {
      if (this.editing_ && e.key === 'Escape') {
        this.finishEditing_();
        e.preventDefault();
      }
      return;
    }
    const mod = e.metaKey || e.ctrlKey;
    const key = e.key.toLowerCase();
    if (mod && key === 'z') {
      e.preventDefault();
      if (e.shiftKey) {
        this.redo_();
      } else {
        this.undo_();
      }
      return;
    }
    if (mod && key === 'y') {
      e.preventDefault();
      this.redo_();
      return;
    }
    if (mod && key === 'a') {
      e.preventDefault();
      this.select_([...this.objects_.keys()]);
      return;
    }
    if (mod && key === 'd') {
      e.preventDefault();
      this.duplicate_();
      return;
    }
    if (mod || e.altKey) {
      return;
    }
    switch (e.key) {
      case 'Delete':
      case 'Backspace':
        e.preventDefault();
        this.deleteSelection_();
        return;
      case 'Escape':
        this.hideMenu_();
        if (this.selection_.size) {
          this.selection_.clear();
          this.updateSelection_();
        } else {
          this.setTool_('select');
        }
        return;
      case 'Enter': {
        const [only] = this.selectedObjects_();
        if (only && this.selection_.size === 1 &&
            only.kind === ObjectKind.kText) {
          e.preventDefault();
          this.startEditing_(only.uuid, false);
        }
        return;
      }
      case ' ':
        e.preventDefault();
        this.spaceDown_ = true;
        this.view_.viewport.classList.add('panning');
        return;
      case ']':
        this.arrange_(true);
        return;
      case '[':
        this.arrange_(false);
        return;
      case '!':
        // Shift+1, as design tools have it.
        this.fit_();
        return;
      case ')':
        this.zoomAround_(this.viewportCenterClient_(), 1);
        return;
      case 'ArrowLeft':
      case 'ArrowRight':
      case 'ArrowUp':
      case 'ArrowDown': {
        if (this.selection_.size === 0) {
          return;
        }
        e.preventDefault();
        const step = e.shiftKey ? 10 : 1;
        const dx = e.key === 'ArrowLeft' ? -step :
            e.key === 'ArrowRight'       ? step :
                                           0;
        const dy = e.key === 'ArrowUp' ? -step :
            e.key === 'ArrowDown'      ? step :
                                         0;
        this.nudge_(dx, dy);
        return;
      }
    }
    if (key === 'i') {
      this.fileInput_?.click();
      return;
    }
    const tool = TOOLS.find(t => t.key === key);
    if (tool) {
      this.setTool_(tool.tool);
    }
  }

  // ---------------------------------------------------------------------
  // The context menu.

  private onContextMenu_(e: MouseEvent) {
    e.preventDefault();
    const uuid = this.uuidAtPoint_(e);
    if (this.gesture_?.type === 'move') {
      // A long press: the menu, not a click.
      this.gesture_.cancelled = true;
    }
    if (uuid && !this.selection_.has(uuid)) {
      this.select_([uuid]);
    }
    const selected = this.selectedObjects_();
    const items: Array<[string, () => void]> = [];
    const [only] = selected;
    if (only && selected.length === 1) {
      const url = urlOf(only);
      if (url) {
        items.push(['Open', () => this.proxy_.handler.openUrl(url)]);
      }
      if (only.kind === ObjectKind.kCapture) {
        items.push(['Refresh', () => void this.refreshCapture_(only.uuid)]);
      }
      if (only.kind === ObjectKind.kText) {
        items.push(['Edit Text', () => this.startEditing_(only.uuid, false)]);
      }
    }
    if (selected.length) {
      items.push(['Bring to Front', () => this.arrange_(true)]);
      items.push(['Send to Back', () => this.arrange_(false)]);
      items.push(['Duplicate', () => this.duplicate_()]);
      items.push(['Delete', () => this.deleteSelection_()]);
    } else {
      items.push(['Add Text Here', () => this.addText_(
                                         this.view_!.toBoard(
                                             e.clientX, e.clientY))]);
      items.push(['Add Image…', () => this.fileInput_?.click()]);
      items.push(['Fit', () => this.fit_()]);
    }
    this.showMenu_(e.clientX, e.clientY, items);
  }

  private showMenu_(
      x: number, y: number, items: Array<[string, () => void]>) {
    const menu = this.menu_!;
    menu.replaceChildren(...items.map(([label, action]) => {
      const item = button(label, 'menu-item', () => {
        this.hideMenu_();
        action();
      });
      item.setAttribute('role', 'menuitem');
      return item;
    }));
    menu.hidden = false;
    const width = menu.offsetWidth;
    const height = menu.offsetHeight;
    menu.style.left = `${Math.min(x, window.innerWidth - width - 8)}px`;
    menu.style.top = `${Math.min(y, window.innerHeight - height - 8)}px`;
    menu.querySelector<HTMLElement>('button')?.focus();
  }

  private hideMenu_() {
    if (this.menu_) {
      this.menu_.hidden = true;
    }
  }

  private async refreshCapture_(uuid: string) {
    toast('Refreshing…');
    const {error} = await this.proxy_.handler.refreshCapture(uuid);
    toast(error || 'Refreshed');
    this.scheduleRefresh_(0);
  }

  // ---------------------------------------------------------------------
  // Selection.

  private selectedObjects_(): EaselObject[] {
    const objects: EaselObject[] = [];
    for (const uuid of this.selection_) {
      const object = this.objects_.get(uuid);
      if (object) {
        objects.push(object);
      }
    }
    return objects;
  }

  private select_(uuids: string[]) {
    this.selection_ = new Set(uuids.filter(u => this.objects_.has(u)));
    this.updateSelection_();
  }

  private updateSelection_() {
    const view = this.view_;
    if (!view) {
      return;
    }
    const rects = this.selectedObjects_().map(rectOf);
    const box = unionRects(rects);
    const marquee = this.gesture_?.type === 'marquee' ?
        rectFromPoints(this.gesture_.start, this.gesture_.current) :
        null;
    const handles = this.tool_ === 'select' && !this.editing_ &&
        this.gesture_?.type !== 'move';
    view.drawSelection(rects, box, handles, marquee);
    this.updateToolbar_();
  }

  // ---------------------------------------------------------------------
  // Saving, and undo.

  // Applies |changes| here and saves them; |record| adds them to the
  // history.
  private commit_(changes: Change[], record = true) {
    const view = this.view_;
    if (!view || changes.length === 0) {
      return;
    }
    const saves: EaselObject[] = [];
    const deletes: string[] = [];
    for (const change of changes) {
      if (change.after) {
        const after = cloneObject(change.after);
        this.objects_.set(after.uuid, after);
        view.update(after);
        saves.push(after);
      } else if (change.before) {
        this.objects_.delete(change.before.uuid);
        this.selection_.delete(change.before.uuid);
        view.remove(change.before.uuid);
        deletes.push(change.before.uuid);
      }
    }
    view.reorder(this.sorted_());
    if (record) {
      this.history_.push(changes);
    }
    this.updateSelection_();
    if (saves.length) {
      void this.save_(saves);
    }
    if (deletes.length) {
      this.proxy_.handler.deleteObjects(deletes);
    }
  }

  private async save_(objects: EaselObject[]) {
    const seq = ++this.lastSaveSeq_;
    for (const object of objects) {
      this.saveSeq_.set(object.uuid, seq);
    }
    const {error, saved} =
        await this.proxy_.handler.saveObjects(this.uuid_, objects);
    if (error) {
      toast(error);
      // Back to what the store has.
      this.scheduleRefresh_(0);
      return;
    }
    const moving = this.gesture_ && 'before' in this.gesture_ ?
        this.gesture_.before :
        new Map<string, EaselObject>();
    for (const object of saved) {
      if (this.saveSeq_.get(object.uuid) !== seq) {
        continue;
      }
      this.saveSeq_.delete(object.uuid);
      if (this.objects_.has(object.uuid) && !moving.has(object.uuid) &&
          this.editing_?.uuid !== object.uuid) {
        // As stored: a long stroke simplified, the base for undo.
        this.objects_.set(object.uuid, object);
        this.view_?.update(object);
      }
    }
  }

  // Saves the objects a gesture changed, from what they were before.
  private commitFrom_(before: Map<string, EaselObject>) {
    const changes: Change[] = [];
    for (const [uuid, old] of before) {
      const now = this.objects_.get(uuid);
      if (now) {
        changes.push({before: old, after: cloneObject(now)});
      }
    }
    this.commit_(changes);
  }

  private add_(objects: EaselObject[], select = true) {
    if (this.objects_.size + objects.length > MAX_OBJECTS) {
      toast('This Easel is full (1,000 objects). Delete some first.');
      return;
    }
    this.commit_(objects.map(o => ({before: null, after: o})));
    if (select) {
      this.select_(objects.map(o => o.uuid));
    }
  }

  private undo_() {
    this.finishEditing_();
    const changes = this.history_.undo();
    if (changes) {
      this.commit_(changes, false);
    }
    this.updateToolbar_();
  }

  private redo_() {
    this.finishEditing_();
    const changes = this.history_.redo();
    if (changes) {
      this.commit_(changes, false);
    }
    this.updateToolbar_();
  }

  // ---------------------------------------------------------------------
  // Edits.

  private change_(objects: EaselObject[], edit: (o: EaselObject) => void) {
    const changes: Change[] = [];
    for (const object of objects) {
      const after = cloneObject(object);
      edit(after);
      changes.push({before: cloneObject(object), after});
    }
    this.commit_(changes);
  }

  private setColor_(color: string) {
    this.color_ = color;
    const colored = this.selectedObjects_().filter(hasColor);
    if (colored.length) {
      this.change_(colored, o => {
        if (o.text) {
          o.text.color = color;
        }
        if (o.shape) {
          o.shape.color = color;
        }
      });
    }
    this.updateToolbar_();
  }

  private setTextField_(field: 'style'|'font', value: string) {
    const texts =
        this.selectedObjects_().filter(o => o.kind === ObjectKind.kText);
    this.change_(texts, o => {
      if (o.text) {
        o.text[field] = value;
      }
    });
    // The box follows the text's new size.
    for (const object of texts) {
      this.fitTextHeight_(object.uuid);
    }
  }

  private toggleFill_() {
    const fillable = this.selectedObjects_().filter(isFillable);
    const filled = !fillable.every(o => o.shape!.filled);
    this.change_(fillable, o => {
      o.shape!.filled = filled;
    });
  }

  private arrange_(front: boolean) {
    const selected = this.selectedObjects_().sort(byZ);
    if (selected.length === 0) {
      return;
    }
    const others = [...this.objects_.values()].filter(
        o => !this.selection_.has(o.uuid));
    let z = front ? maxZ(others) + 1 : minZ(others) - selected.length;
    this.change_(selected, o => {
      o.z = z++;
    });
  }

  private duplicate_() {
    const selected = this.selectedObjects_().sort(byZ);
    if (selected.length === 0) {
      return;
    }
    let z = maxZ(this.objects_.values()) + 1;
    // A copy starts from its original's base: what the page can't see (a
    // capture's region) comes along.
    this.add_(selected.map(o => ({
                             ...cloneObject(o),
                             uuid: newUuid(),
                             x: o.x + 24,
                             y: o.y + 24,
                             z: z++,
                           })));
  }

  private deleteSelection_() {
    const selected = this.selectedObjects_();
    this.commit_(selected.map(o => ({before: cloneObject(o), after: null})));
  }

  private nudge_(dx: number, dy: number) {
    this.change_(this.selectedObjects_(), o => {
      o.x += dx;
      o.y += dy;
    });
  }

  // ---------------------------------------------------------------------
  // Adding objects.

  private addText_(at: Point, text = '') {
    const object = blankObject(
        ObjectKind.kText, at.x, at.y - 14, text ? 360 : 240, 32,
        maxZ(this.objects_.values()) + 1);
    object.text = {text, color: this.color_, font: '', style: ''};
    if (text) {
      this.objects_.set(object.uuid, object);
      this.view_!.update(object);
      this.fitTextHeight_(object.uuid, /*save=*/ false);
      this.objects_.delete(object.uuid);
      this.view_!.remove(object.uuid);
      this.add_([object]);
      return;
    }
    // Shown, but saved only once it has text.
    this.objects_.set(object.uuid, object);
    this.view_!.update(object);
    this.select_([object.uuid]);
    this.startEditing_(object.uuid, true);
  }

  private addShape_(kind: ShapeKind, start: Point, end: Point) {
    const view = this.view_!;
    let a = start;
    let b = end;
    if (distance(a, b) * view.zoom < CLICK_SLOP) {
      // A click: a shape of a usual size there.
      b = kind === ShapeKind.ARROW ? {x: a.x + 160, y: a.y} :
                                     {x: a.x + 160, y: a.y + 100};
      if (kind !== ShapeKind.ARROW) {
        a = {x: a.x - 80, y: a.y - 50};
        b = {x: b.x - 80, y: b.y - 50};
      }
    }
    const z = maxZ(this.objects_.values()) + 1;
    let object: EaselObject;
    if (kind === ShapeKind.ARROW) {
      const pad = DEFAULT_STROKE_WIDTH * 4;
      const {box, points} = boxAround([a.x, a.y, b.x, b.y], pad);
      object = blankObject(ObjectKind.kShape, box.x, box.y, box.width,
                           box.height, z);
      object.shape = shapeContent(kind, this.color_, points);
    } else {
      const box = rectFromPoints(a, b);
      object = blankObject(ObjectKind.kShape, box.x, box.y, box.width,
                           box.height, z);
      object.shape = shapeContent(kind, this.color_, []);
    }
    this.add_([object]);
    this.setTool_('select');
    this.select_([object.uuid]);
  }

  private drawPenDraft_() {
    const gesture = this.gesture_;
    if (gesture?.type !== 'pen') {
      return;
    }
    this.view_!.drawDraft({
      kind: ShapeKind.FREEHAND,
      rect: {x: 0, y: 0, width: 0, height: 0},
      points: gesture.points,
      color: this.color_,
    });
  }

  private addStroke_(points: number[]) {
    if (points.length < 2) {
      return;
    }
    const {box, points: relative} =
        boxAround(points, DEFAULT_STROKE_WIDTH / 2 + 1);
    const object = blankObject(ObjectKind.kShape, box.x, box.y, box.width,
                               box.height, maxZ(this.objects_.values()) + 1);
    object.shape = shapeContent(ShapeKind.FREEHAND, this.color_, relative);
    // The pen keeps drawing; the stroke isn't selected.
    this.add_([object], /*select=*/ false);
  }

  private async addFiles_(files: File[], at: Point) {
    let offset = 0;
    for (const file of files) {
      if (!file.type.startsWith('image/')) {
        continue;
      }
      if (file.size > 32 * 1024 * 1024) {
        toast('The image is too large.');
        continue;
      }
      const bytes = new Uint8Array(await file.arrayBuffer());
      // The bindings take a typed array for array<uint8> as they do an
      // Array, without copying it into one.
      const {error, sha256, width, height} =
          await this.proxy_.handler.addImage(bytes as unknown as number[]);
      if (error) {
        toast(error);
        continue;
      }
      const scale = Math.min(1, IMAGE_SIZE / Math.max(width, height, 1));
      const w = Math.max(1, width * scale);
      const h = Math.max(1, height * scale);
      const object = blankObject(
          ObjectKind.kImage, at.x - w / 2 + offset, at.y - h / 2 + offset, w,
          h, maxZ(this.objects_.values()) + 1);
      object.image = {asset: sha256, pixelWidth: width, pixelHeight: height};
      this.add_([object]);
      offset += 24;
    }
  }

  private async addLink_(url: string, at: Point) {
    toast('Adding link…');
    const {link} = await this.proxy_.handler.addLink(url);
    if (!link) {
      this.addText_(at, url);
      return;
    }
    this.addLinkCard_(link, at);
  }

  private addLinkCard_(link: LinkContent, at: Point) {
    const width = 300;
    const height = link.imageAsset ? 280 : (link.description ? 130 : 96);
    const object = blankObject(
        ObjectKind.kLink, at.x - width / 2, at.y - height / 2, width, height,
        maxZ(this.objects_.values()) + 1);
    object.link = link;
    this.add_([object]);
  }

  // Pasted or dropped text: a link card for a web address, else text.
  private addPlainText_(text: string, at: Point) {
    const url = asWebUrl(text);
    if (url) {
      void this.addLink_(url, at);
      return;
    }
    const trimmed = text.trim().slice(0, MAX_TEXT_CHARS);
    if (trimmed) {
      this.addText_(at, trimmed);
    }
  }

  private pastePoint_(): Point {
    const view = this.view_!;
    const r = view.viewport.getBoundingClientRect();
    const p = this.lastPoint_;
    if (p) {
      const s = view.toScreen(p);
      if (s.x >= 0 && s.y >= 0 && s.x <= r.width && s.y <= r.height) {
        return p;
      }
    }
    return view.center();
  }

  private isTyping_(): boolean {
    const active = document.activeElement;
    return !!this.editing_ ||
        (active instanceof HTMLElement &&
         (active.isContentEditable || active instanceof HTMLInputElement ||
          active instanceof HTMLTextAreaElement));
  }

  private onPaste_(e: ClipboardEvent) {
    if (!this.view_ || this.isTyping_() || !e.clipboardData) {
      return;
    }
    e.preventDefault();
    const data = e.clipboardData;
    const at = this.pastePoint_();
    const copied = data.getData(CLIPBOARD_TYPE);
    if (copied && this.pasteObjects_(copied)) {
      return;
    }
    const files = [...data.files].filter(f => f.type.startsWith('image/'));
    if (files.length) {
      void this.addFiles_(files, at);
      return;
    }
    const text = data.getData('text/plain');
    if (text) {
      this.addPlainText_(text, at);
    }
  }

  private pasteObjects_(json: string): boolean {
    let parsed: unknown;
    try {
      parsed = JSON.parse(json);
    } catch {
      return false;
    }
    if (!Array.isArray(parsed)) {
      return false;
    }
    let z = maxZ(this.objects_.values()) + 1;
    const objects: EaselObject[] = [];
    for (const item of parsed as EaselObject[]) {
      if (typeof item.uuid !== 'string' || typeof item.kind !== 'number' ||
          typeof item.base !== 'string') {
        continue;
      }
      objects.push(
          {...item, uuid: newUuid(), x: item.x + 24, y: item.y + 24, z: z++});
    }
    if (objects.length === 0) {
      return false;
    }
    this.add_(objects);
    return true;
  }

  private onCopy_(e: ClipboardEvent, cut: boolean) {
    if (!this.view_ || this.isTyping_() || !e.clipboardData) {
      return;
    }
    const selected = this.selectedObjects_().sort(byZ);
    if (selected.length === 0) {
      return;
    }
    e.preventDefault();
    e.clipboardData.setData(CLIPBOARD_TYPE, JSON.stringify(selected));
    const text = selected
                     .map(o => o.text ? o.text.text : urlOf(o))
                     .filter(t => t)
                     .join('\n\n');
    if (text) {
      e.clipboardData.setData('text/plain', text);
    }
    if (cut) {
      this.deleteSelection_();
    }
  }

  private onDrop_(e: DragEvent) {
    e.preventDefault();
    const data = e.dataTransfer;
    if (!data || !this.view_) {
      return;
    }
    const at = this.view_.toBoard(e.clientX, e.clientY);
    const files = [...data.files].filter(f => f.type.startsWith('image/'));
    if (files.length) {
      void this.addFiles_(files, at);
      return;
    }
    const uri = data.getData('text/uri-list')
                    .split(/\r?\n/)
                    .find(line => line && !line.startsWith('#'));
    if (uri && asWebUrl(uri)) {
      void this.addLink_(uri, at);
      return;
    }
    const text = data.getData('text/plain');
    if (text) {
      this.addPlainText_(text, at);
    }
  }

  // ---------------------------------------------------------------------
  // Editing text in place.

  private startEditing_(uuid: string, isNew: boolean) {
    const view = this.view_!;
    const object = this.objects_.get(uuid);
    const body = view.textBody(uuid);
    const element = view.element(uuid);
    if (!object || !body || !element) {
      return;
    }
    this.finishEditing_();
    this.editing_ = {uuid, isNew, before: isNew ? null : cloneObject(object)};
    element.classList.add('editing');
    body.contentEditable = 'plaintext-only';
    body.spellcheck = true;
    body.focus();
    if (!isNew) {
      const range = document.createRange();
      range.selectNodeContents(body);
      const selection = window.getSelection();
      selection?.removeAllRanges();
      selection?.addRange(range);
    }
    body.addEventListener('blur', () => this.finishEditing_(), {once: true});
    this.updateSelection_();
  }

  private finishEditing_() {
    const editing = this.editing_;
    if (!editing) {
      return;
    }
    this.editing_ = null;
    const view = this.view_;
    const object = this.objects_.get(editing.uuid);
    const body = view?.textBody(editing.uuid);
    const element = view?.element(editing.uuid);
    if (!view || !object || !body || !element) {
      return;
    }
    const text = body.innerText.replace(/\n$/, '').slice(0, MAX_TEXT_CHARS);
    body.contentEditable = 'false';
    element.classList.remove('editing');
    window.getSelection()?.removeAllRanges();
    if (!text.trim()) {
      // Emptied: gone.
      if (editing.isNew) {
        this.objects_.delete(object.uuid);
        this.selection_.delete(object.uuid);
        view.remove(object.uuid);
        this.updateSelection_();
      } else if (editing.before) {
        this.commit_([{before: editing.before, after: null}]);
      }
      return;
    }
    const after = cloneObject(object);
    after.text!.text = text;
    after.height = Math.max(8, Math.ceil(body.offsetHeight));
    if (!editing.isNew && editing.before &&
        editing.before.text?.text === text &&
        editing.before.height === after.height) {
      this.updateSelection_();
      return;
    }
    this.commit_([{before: editing.before, after}]);
  }

  // Sets a text's height to its text's, once laid out.
  private fitTextHeight_(uuid: string, save = true) {
    const body = this.view_?.textBody(uuid);
    const object = this.objects_.get(uuid);
    if (!body || !object) {
      return;
    }
    const height = Math.max(8, Math.ceil(body.offsetHeight));
    if (Math.abs(height - object.height) < 1) {
      return;
    }
    if (!save) {
      object.height = height;
      return;
    }
    const before = cloneObject(object);
    const after = cloneObject(object);
    after.height = height;
    // Part of the edit that changed the text's size, not a step of its own.
    this.commit_([{before, after}], /*record=*/ false);
  }
}

function isFillable(object: EaselObject): boolean {
  return !!object.shape &&
      (object.shape.kind === ShapeKind.RECTANGLE ||
       object.shape.kind === ShapeKind.CIRCLE);
}

function shapeContent(kind: ShapeKind, color: string, points: number[]) {
  return {kind, color, points, strokeWidth: 0, filled: false};
}

// Shift while drawing: a square, a circle, or an arrow at 45° steps.
function constrain(
    gesture: Extract<Gesture, {type: 'draw'}>, point: Point): Point {
  const dx = point.x - gesture.start.x;
  const dy = point.y - gesture.start.y;
  if (gesture.kind === ShapeKind.ARROW) {
    const angle = Math.round(Math.atan2(dy, dx) / (Math.PI / 4)) *
        (Math.PI / 4);
    const length = Math.hypot(dx, dy);
    return {
      x: gesture.start.x + length * Math.cos(angle),
      y: gesture.start.y + length * Math.sin(angle),
    };
  }
  const size = Math.max(Math.abs(dx), Math.abs(dy));
  return {
    x: gesture.start.x + Math.sign(dx || 1) * size,
    y: gesture.start.y + Math.sign(dy || 1) * size,
  };
}
