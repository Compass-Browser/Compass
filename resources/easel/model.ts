// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The page's side of a board's objects: the mojom structs as they come and
// go (EaselObject), and how to make and read them.

import {ObjectKind} from './easel.mojom-webui.js';
import type {EaselObject} from './easel.mojom-webui.js';

// CompassEaselShape.kind.
export enum ShapeKind {
  RECTANGLE = 1,
  CIRCLE = 2,
  ARROW = 3,
  FREEHAND = 4,
}

// The board backgrounds (CompassEasel.palette); anything else is paper.
export const PALETTES: Array<{value: string, name: string}> = [
  {value: 'paper', name: 'Paper'},
  {value: 'grid', name: 'Grid'},
  {value: 'dots', name: 'Dots'},
  {value: 'dark', name: 'Dark'},
];

export function paletteOf(value: string): string {
  return PALETTES.some(p => p.value === value) ? value : 'paper';
}

// The palette for text and shapes, as iOS has it (the same hex strings go
// in the messages, so boards look alike on every device). '' is the board's ink (absent in the
// message), which follows the background.
export const COLORS: Array<{value: string, name: string}> = [
  {value: '', name: 'Ink'},
  {value: '#d1242f', name: 'Red'},
  {value: '#e16f24', name: 'Orange'},
  {value: '#d4a72c', name: 'Yellow'},
  {value: '#1a7f37', name: 'Green'},
  {value: '#0969da', name: 'Blue'},
  {value: '#8250df', name: 'Purple'},
  {value: '#6e7781', name: 'Grey'},
];

export const TEXT_STYLES: Array<{value: string, name: string}> = [
  {value: 'title', name: 'Title'},
  {value: 'heading', name: 'Heading'},
  {value: '', name: 'Body'},
  {value: 'caption', name: 'Caption'},
];

export const FONTS: Array<{value: string, name: string}> = [
  {value: '', name: 'Sans'},
  {value: 'serif', name: 'Serif'},
  {value: 'mono', name: 'Mono'},
  {value: 'hand', name: 'Hand'},
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
