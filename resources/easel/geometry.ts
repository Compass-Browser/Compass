// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {EaselObject} from './easel.mojom-webui.js';

export interface Point {
  x: number;
  y: number;
}

export interface Rect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export function rectOf(object: EaselObject): Rect {
  return {
    x: object.x,
    y: object.y,
    width: object.width,
    height: object.height,
  };
}

export function unionRects(rects: Rect[]): Rect|null {
  if (rects.length === 0) {
    return null;
  }
  let left = Infinity;
  let top = Infinity;
  let right = -Infinity;
  let bottom = -Infinity;
  for (const r of rects) {
    left = Math.min(left, r.x);
    top = Math.min(top, r.y);
    right = Math.max(right, r.x + r.width);
    bottom = Math.max(bottom, r.y + r.height);
  }
  return {x: left, y: top, width: right - left, height: bottom - top};
}

export function rectFromPoints(a: Point, b: Point): Rect {
  return {
    x: Math.min(a.x, b.x),
    y: Math.min(a.y, b.y),
    width: Math.abs(a.x - b.x),
    height: Math.abs(a.y - b.y),
  };
}

export function intersects(a: Rect, b: Rect): boolean {
  return a.x <= b.x + b.width && b.x <= a.x + a.width &&
      a.y <= b.y + b.height && b.y <= a.y + a.height;
}

export function distance(a: Point, b: Point): number {
  return Math.hypot(a.x - b.x, a.y - b.y);
}

// A smooth path through a stroke's points (x, y pairs): quadratic curves
// through the midpoints, so the pen's samples don't show as corners.
export function strokePath(points: number[]): string {
  const n = Math.floor(points.length / 2);
  if (n === 0) {
    return '';
  }
  const x = (i: number) => points[2 * i] ?? 0;
  const y = (i: number) => points[2 * i + 1] ?? 0;
  const round = (v: number) => Math.round(v * 100) / 100;
  if (n === 1) {
    // A dot: a zero-length line with round caps.
    return `M${round(x(0))} ${round(y(0))}l0.01 0`;
  }
  const parts = [`M${round(x(0))} ${round(y(0))}`];
  for (let i = 1; i < n - 1; i++) {
    const mx = (x(i) + x(i + 1)) / 2;
    const my = (y(i) + y(i + 1)) / 2;
    parts.push(`Q${round(x(i))} ${round(y(i))} ${round(mx)} ${round(my)}`);
  }
  parts.push(`L${round(x(n - 1))} ${round(y(n - 1))}`);
  return parts.join('');
}

// An arrow from (x1, y1) to (x2, y2): its line, stopping short of the tip so
// round caps don't poke through, and its filled head.
export function arrowGeometry(
    points: number[], strokeWidth: number): {line: string, head: string} {
  const [x1 = 0, y1 = 0, x2 = 0, y2 = 0] = points;
  const angle = Math.atan2(y2 - y1, x2 - x1);
  const length = Math.hypot(x2 - x1, y2 - y1);
  const headLength = Math.min(Math.max(12, strokeWidth * 4), length * 0.6);
  const spread = Math.PI / 7;
  const hx1 = x2 - headLength * Math.cos(angle - spread);
  const hy1 = y2 - headLength * Math.sin(angle - spread);
  const hx2 = x2 - headLength * Math.cos(angle + spread);
  const hy2 = y2 - headLength * Math.sin(angle + spread);
  const shorten = headLength * 0.7;
  const lx = x2 - shorten * Math.cos(angle);
  const ly = y2 - shorten * Math.sin(angle);
  return {
    line: `M${x1} ${y1}L${lx} ${ly}`,
    head: `M${x2} ${y2}L${hx1} ${hy1}L${hx2} ${hy2}Z`,
  };
}

// Points of a shape scaled from its old box size to its new one.
export function scalePoints(
    points: number[], sx: number, sy: number): number[] {
  return points.map((v, i) => i % 2 === 0 ? v * sx : v * sy);
}

// A stroke (absolute board points) as an object's box and points relative
// to it, with room for the line's width.
export function boxAround(
    points: number[], pad: number): {box: Rect, points: number[]} {
  let left = Infinity;
  let top = Infinity;
  let right = -Infinity;
  let bottom = -Infinity;
  for (let i = 0; i + 1 < points.length; i += 2) {
    const x = points[i] ?? 0;
    const y = points[i + 1] ?? 0;
    left = Math.min(left, x);
    right = Math.max(right, x);
    top = Math.min(top, y);
    bottom = Math.max(bottom, y);
  }
  left -= pad;
  top -= pad;
  right += pad;
  bottom += pad;
  return {
    box: {x: left, y: top, width: right - left, height: bottom - top},
    points: points.map((v, i) => i % 2 === 0 ? v - left : v - top),
  };
}
