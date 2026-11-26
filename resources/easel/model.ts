// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The page's side of a board's objects: the mojom structs as they come and
// go (EaselObject), and how to make and read them.

import {ObjectKind} from './easel.mojom-webui.js';
import type {EaselObject} from './easel.mojom-webui.js';
import {i18n} from './i18n.js';

// CompassEaselShape.kind.
export enum ShapeKind {
  RECTANGLE = 1,
  CIRCLE = 2,
  ARROW = 3,
  FREEHAND = 4,
}

// The board backgrounds (CompassEasel.palette); anything else is paper.
export const PALETTES: Array<{value: string, name: string}> = [
  {value: 'paper', name: i18n('easelPalettePaper')},
  {value: 'grid', name: i18n('easelPaletteGrid')},
  {value: 'dots', name: i18n('easelPaletteDots')},
  {value: 'dark', name: i18n('easelPaletteDark')},
];

export function paletteOf(value: string): string {
  return PALETTES.some(p => p.value === value) ? value : 'paper';
}

// The palette for text and shapes, as iOS has it (the same hex strings go
// in the messages, so boards look alike on every device). '' is the board's ink (absent in the
// message), which follows the background.
export const COLORS: Array<{value: string, name: string}> = [
  {value: '', name: i18n('easelColorInk')},
  {value: '#d1242f', name: i18n('easelColorRed')},
  {value: '#e16f24', name: i18n('easelColorOrange')},
  {value: '#d4a72c', name: i18n('easelColorYellow')},
  {value: '#1a7f37', name: i18n('easelColorGreen')},
  {value: '#0969da', name: i18n('easelColorBlue')},
  {value: '#8250df', name: i18n('easelColorPurple')},
  {value: '#6e7781', name: i18n('easelColorGrey')},
];

export const TEXT_STYLES: Array<{value: string, name: string}> = [
  {value: 'title', name: i18n('easelStyleTitle')},
  {value: 'heading', name: i18n('easelStyleHeading')},
  {value: '', name: i18n('easelStyleBody')},
  {value: 'caption', name: i18n('easelStyleCaption')},
];

export const FONTS: Array<{value: string, name: string}> = [
  {value: '', name: i18n('easelFontSans')},
  {value: 'serif', name: i18n('easelFontSerif')},
  {value: 'mono', name: i18n('easelFontMono')},
  {value: 'hand', name: i18n('easelFontHand')},
];

// docs/design/easels.md, "What a board is".
export const MAX_OBJECTS = 1000;
export const MAX_TEXT_CHARS = 20000;
export const DEFAULT_STROKE_WIDTH = 3;

export function newUuid(): string {
  return crypto.randomUUID();
}

export function cloneObject(object: EaselObject): EaselObject {
  return structuredClone(object);
}

// An object of |kind| with no content yet; the caller sets it.
export function blankObject(
    kind: ObjectKind, x: number, y: number, width: number, height: number,
    z: number): EaselObject {
  return {
    uuid: newUuid(),
    x,
    y,
    width,
    height,
    z,
    kind,
    text: null,
    shape: null,
    image: null,
    link: null,
    capture: null,
    base: '',
  };
}

// The page a link or capture opens; '' for other kinds.
export function urlOf(object: EaselObject): string {
  if (object.kind === ObjectKind.kLink && object.link) {
    return object.link.url;
  }
  if (object.kind === ObjectKind.kCapture && object.capture) {
    return object.capture.url;
  }
  return '';
}

export function hostOf(url: string): string {
  try {
    return new URL(url).host;
  } catch {
    return url;
  }
}

export function assetUrl(sha256: string): string {
  return `/asset/${sha256}`;
}

export function hasColor(object: EaselObject): boolean {
  return object.kind === ObjectKind.kText || object.kind === ObjectKind.kShape;
}

export function colorOf(object: EaselObject): string {
  if (object.text) {
    return object.text.color;
  }
  if (object.shape) {
    return object.shape.color;
  }
  return '';
}

export function maxZ(objects: Iterable<EaselObject>): number {
  let z = 0;
  let any = false;
  for (const object of objects) {
    z = any ? Math.max(z, object.z) : object.z;
    any = true;
  }
  return z;
}

export function minZ(objects: Iterable<EaselObject>): number {
  let z = 0;
  let any = false;
  for (const object of objects) {
    z = any ? Math.min(z, object.z) : object.z;
    any = true;
  }
  return z;
}

export function byZ(a: EaselObject, b: EaselObject): number {
  return a.z - b.z || (a.uuid < b.uuid ? -1 : a.uuid > b.uuid ? 1 : 0);
}

// A pasted string that is a single web address, as a URL; else null.
export function asWebUrl(text: string): string|null {
  const trimmed = text.trim();
  if (!trimmed || /\s/.test(trimmed)) {
    return null;
  }
  const candidate = /^www\.[^.]+\./i.test(trimmed) ? `https://${trimmed}` :
                                                     trimmed;
  try {
    const url = new URL(candidate);
    return url.protocol === 'http:' || url.protocol === 'https:' ? url.href :
                                                                   null;
  } catch {
    return null;
  }
}
