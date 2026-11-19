// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Draws a board: its objects as positioned elements in a world layer that
// pan and zoom only transform, and the selection on top in screen points.
// An object's element is rebuilt only when its content changes; a move only
// sets its transform, so dragging many objects stays smooth.

import {el, svg} from './dom.js';
import {ObjectKind} from './easel.mojom-webui.js';
import type {EaselObject} from './easel.mojom-webui.js';
import type {Point, Rect} from './geometry.js';
import {arrowGeometry, strokePath} from './geometry.js';
import {assetUrl, DEFAULT_STROKE_WIDTH, hostOf, ShapeKind} from './model.js';

export const HANDLES = ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w'];

const MIN_ZOOM = 0.1;
const MAX_ZOOM = 8;

export function clampZoom(zoom: number): number {
  return Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, zoom));
}

export class BoardView {
  readonly viewport: HTMLElement;
  readonly world: HTMLElement;
  private readonly overlay_: HTMLElement;
  private readonly draft_: SVGSVGElement;
  private elements_ = new Map<string, HTMLElement>();
  // What each element was built from, so an unchanged object isn't rebuilt.
  private keys_ = new Map<string, string>();
  panX = 0;
  panY = 0;
  zoom = 1;

  constructor(viewport: HTMLElement) {
    this.viewport = viewport;
    this.world = el('div', {className: 'world'});
    this.draft_ = svg('svg', {'class': 'draft'});
    this.world.appendChild(this.draft_);
    this.overlay_ = el('div', {className: 'overlay'});
    viewport.append(this.world, this.overlay_);
  }

  setPalette(palette: string) {
    this.viewport.dataset['palette'] = palette;
  }

  setView(panX: number, panY: number, zoom: number) {
    this.panX = panX;
    this.panY = panY;
    this.zoom = clampZoom(zoom);
    this.world.style.transform =
        `translate(${this.panX}px, ${this.panY}px) scale(${this.zoom})`;
    // The grid and dots move with the board.
    const style = this.viewport.style;
    style.setProperty('--grid-size', `${24 * this.zoom}px`);
    style.setProperty('--grid-x', `${this.panX}px`);
    style.setProperty('--grid-y', `${this.panY}px`);
  }

  // A point of the page (clientX, clientY) on the board, and back
  // (relative to the viewport).
  toBoard(clientX: number, clientY: number): Point {
    const r = this.viewport.getBoundingClientRect();
    return {
      x: (clientX - r.left - this.panX) / this.zoom,
      y: (clientY - r.top - this.panY) / this.zoom,
    };
  }

  toScreen(point: Point): Point {
    return {
      x: point.x * this.zoom + this.panX,
      y: point.y * this.zoom + this.panY,
    };
  }

  screenRect(rect: Rect): Rect {
    const p = this.toScreen(rect);
    return {
      x: p.x,
      y: p.y,
      width: rect.width * this.zoom,
      height: rect.height * this.zoom,
    };
  }

  // The board point at the middle of what's visible.
  center(): Point {
    const r = this.viewport.getBoundingClientRect();
    return this.toBoard(r.left + r.width / 2, r.top + r.height / 2);
  }

  // The object an event's target belongs to.
  uuidAt(target: EventTarget|null): string|null {
    if (!(target instanceof Element)) {
      return null;
    }
    const element = target.closest<HTMLElement>('.obj');
    return element ? element.dataset['uuid'] || null : null;
  }

  element(uuid: string): HTMLElement|null {
    return this.elements_.get(uuid) || null;
  }

  textBody(uuid: string): HTMLElement|null {
    const element = this.elements_.get(uuid);
    return element ? element.querySelector<HTMLElement>('.text-body') : null;
  }

  // Shows exactly |objects|, lowest z first.
  render(objects: EaselObject[]) {
    const seen = new Set<string>();
    for (const object of objects) {
      seen.add(object.uuid);
      this.update(object);
    }
    for (const uuid of [...this.elements_.keys()]) {
      if (!seen.has(uuid)) {
        this.remove(uuid);
      }
    }
    this.reorder(objects);
  }

  // Stacks the elements as |objects| (lowest z first) are.
  reorder(objects: EaselObject[]) {
    let previous: Element = this.draft_;
    // Every object goes before the draft, which stays on top.
    for (let i = objects.length - 1; i >= 0; i--) {
      const object = objects[i];
      const element = object ? this.elements_.get(object.uuid) : undefined;
      if (!element) {
        continue;
      }
      if (element.nextElementSibling !== previous) {
        this.world.insertBefore(element, previous);
      }
      previous = element;
    }
  }

  // Adds or redraws one object (new ones on top until reorder()).
  update(object: EaselObject) {
    const key = contentKey(object);
    let element = this.elements_.get(object.uuid);
    if (!element) {
      element = el('div', {className: 'obj'});
      element.dataset['uuid'] = object.uuid;
      this.world.insertBefore(element, this.draft_);
      this.elements_.set(object.uuid, element);
    }
    if (this.keys_.get(object.uuid) !== key) {
      this.keys_.set(object.uuid, key);
      element.dataset['kind'] = kindName(object.kind);
      element.replaceChildren(buildContent(object));
    }
    this.place(object);
  }

  // Only the box: a move, or a resize of a kind whose drawing doesn't
  // depend on its size.
  place(object: EaselObject) {
    const element = this.elements_.get(object.uuid);
    if (!element) {
      return;
    }
    element.style.transform = `translate(${object.x}px, ${object.y}px)`;
    element.style.width = `${object.width}px`;
    element.style.height = `${object.height}px`;
  }

  remove(uuid: string) {
    this.elements_.get(uuid)?.remove();
    this.elements_.delete(uuid);
    this.keys_.delete(uuid);
  }

  // A shape being drawn, in board points; null clears it.
  drawDraft(shape: {kind: ShapeKind, rect: Rect, points: number[],
                    color: string}|null) {
    this.draft_.replaceChildren();
    if (!shape) {
      return;
    }
    this.draft_.style.color = shape.color;
    for (const node of shapeNodes(
             shape.kind, shape.rect, shape.points, DEFAULT_STROKE_WIDTH,
             false)) {
      this.draft_.appendChild(node);
    }
  }

  // The selection: each object's outline, the box around them with its
  // handles (|handles|), and a marquee being dragged.
  drawSelection(
      outlines: Rect[], box: Rect|null, handles: boolean,
      marquee: Rect|null) {
    const nodes: HTMLElement[] = [];
    const place = (element: HTMLElement, r: Rect) => {
      const s = this.screenRect(r);
      element.style.transform = `translate(${s.x}px, ${s.y}px)`;
      element.style.width = `${s.width}px`;
      element.style.height = `${s.height}px`;
      nodes.push(element);
    };
    if (outlines.length > 1 && outlines.length <= 200) {
      for (const r of outlines) {
        place(el('div', {className: 'outline'}), r);
      }
    }
    if (box) {
      place(el('div', {className: 'selection-box'}), box);
      if (handles) {
        const s = this.screenRect(box);
        for (const handle of HANDLES) {
          const h = el('div', {className: 'handle'});
          h.dataset['handle'] = handle;
          const x = handle.includes('w') ? s.x :
              handle.includes('e')       ? s.x + s.width :
                                           s.x + s.width / 2;
          const y = handle.includes('n') ? s.y :
              handle.includes('s')       ? s.y + s.height :
                                           s.y + s.height / 2;
          h.style.transform = `translate(${x}px, ${y}px)`;
          nodes.push(h);
        }
      }
    }
    if (marquee) {
      place(el('div', {className: 'marquee'}), marquee);
    }
    this.overlay_.replaceChildren(...nodes);
  }
}

function kindName(kind: ObjectKind): string {
  switch (kind) {
    case ObjectKind.kText:
      return 'text';
    case ObjectKind.kShape:
      return 'shape';
    case ObjectKind.kImage:
      return 'image';
    case ObjectKind.kLink:
      return 'link';
    case ObjectKind.kCapture:
      return 'capture';
    default:
      return 'unknown';
  }
}

// Shapes are drawn for their size; other kinds stretch with CSS.
function contentKey(object: EaselObject): string {
  switch (object.kind) {
    case ObjectKind.kText:
      return JSON.stringify(['t', object.text]);
    case ObjectKind.kShape:
      return JSON.stringify(
          ['s', object.shape, object.width, object.height]);
    case ObjectKind.kImage:
      return JSON.stringify(['i', object.image]);
    case ObjectKind.kLink:
      return JSON.stringify(['l', object.link]);
    case ObjectKind.kCapture:
      return JSON.stringify(['c', object.capture]);
    default:
      return 'u';
  }
}

function buildContent(object: EaselObject): Node {
  switch (object.kind) {
    case ObjectKind.kText:
      return buildText(object);
    case ObjectKind.kShape:
      return buildShape(object);
    case ObjectKind.kImage:
      return buildImage(object);
    case ObjectKind.kLink:
      return buildLink(object);
    case ObjectKind.kCapture:
      return buildCapture(object);
    default:
      return el('div', {
        className: 'unknown',
        text: 'Made with a newer version of Compass',
      });
  }
}

function buildText(object: EaselObject): Node {
  const text = object.text;
  const body = el('div', {className: 'text-body', text: text?.text || ''});
  body.dataset['font'] = text?.font || 'sans';
  body.dataset['style'] = text?.style || 'body';
  if (text?.color) {
    body.style.color = text.color;
  }
  return body;
}

function shapeNodes(
    kind: ShapeKind, rect: Rect, points: number[], strokeWidth: number,
    filled: boolean): SVGElement[] {
  const sw = strokeWidth;
  const fill = filled ? 'currentColor' : 'none';
  switch (kind) {
    case ShapeKind.RECTANGLE:
      return [svg('rect', {
        'x': rect.x + sw / 2,
        'y': rect.y + sw / 2,
        'width': Math.max(0, rect.width - sw),
        'height': Math.max(0, rect.height - sw),
        'rx': Math.min(6, rect.width / 4, rect.height / 4),
        'fill': fill,
        'stroke': 'currentColor',
        'stroke-width': sw,
      })];
    case ShapeKind.CIRCLE:
      return [svg('ellipse', {
        'cx': rect.x + rect.width / 2,
        'cy': rect.y + rect.height / 2,
        'rx': Math.max(0, (rect.width - sw) / 2),
        'ry': Math.max(0, (rect.height - sw) / 2),
        'fill': fill,
        'stroke': 'currentColor',
        'stroke-width': sw,
      })];
    case ShapeKind.ARROW: {
      const {line, head} = arrowGeometry(points, sw);
      return [
        svg('path', {
          'd': line,
          'fill': 'none',
          'stroke': 'currentColor',
          'stroke-width': sw,
          'stroke-linecap': 'round',
        }),
        svg('path', {
          'd': head,
          'fill': 'currentColor',
          'stroke': 'currentColor',
          'stroke-width': sw,
          'stroke-linejoin': 'round',
        }),
      ];
    }
    case ShapeKind.FREEHAND:
      return [svg('path', {
        'd': strokePath(points),
        'fill': 'none',
        'stroke': 'currentColor',
        'stroke-width': sw,
        'stroke-linecap': 'round',
        'stroke-linejoin': 'round',
      })];
    default:
      return [];
  }
}

function buildShape(object: EaselObject): Node {
  const shape = object.shape;
  const width = object.width;
  const height = object.height;
  const root = svg('svg', {
    'class': 'shape',
    'width': width,
    'height': height,
    'viewBox': `0 0 ${width} ${height}`,
  });
  if (!shape) {
    return root;
  }
  if (shape.color) {
    root.style.color = shape.color;
  }
  for (const node of shapeNodes(
           shape.kind, {x: 0, y: 0, width, height}, shape.points,
           shape.strokeWidth > 0 ? shape.strokeWidth : DEFAULT_STROKE_WIDTH,
           shape.filled)) {
    root.appendChild(node);
  }
  return root;
}

function image(sha256: string, className: string): HTMLImageElement {
  const img = el('img', {className, attrs: {'alt': '', 'draggable': 'false'}});
  img.decoding = 'async';
  img.src = assetUrl(sha256);
  return img;
}

function buildImage(object: EaselObject): Node {
  return object.image ? image(object.image.asset, 'picture') :
                        el('div', {className: 'unknown'});
}

function buildLink(object: EaselObject): Node {
  const link = object.link;
  const card = el('div', {className: 'card'});
  if (!link) {
    return card;
  }
  const host = hostOf(link.url);
  card.title = link.url;
  if (link.imageAsset) {
    card.appendChild(image(link.imageAsset, 'card-image'));
  }
  const body = el('div', {className: 'card-body'}, [
    el('div', {className: 'card-title', text: link.title || host}),
  ]);
  if (link.description) {
    body.appendChild(
        el('div', {className: 'card-description', text: link.description}));
  }
  body.appendChild(el('div', {className: 'card-host', text: host}));
  card.appendChild(body);
  return card;
}

function buildCapture(object: EaselObject): Node {
  const capture = object.capture;
  const frame = el('div', {className: 'capture'});
  if (!capture) {
    return frame;
  }
  frame.title = capture.url;
  if (capture.imageAsset) {
    frame.appendChild(image(capture.imageAsset, 'capture-image'));
  }
  const bar = el('div', {className: 'capture-bar'});
  if (capture.faviconAsset) {
    bar.appendChild(image(capture.faviconAsset, 'capture-icon'));
  }
  bar.appendChild(el('span', {
    className: 'capture-title',
    text: capture.title || hostOf(capture.url),
  }));
  frame.appendChild(bar);
  return frame;
}
