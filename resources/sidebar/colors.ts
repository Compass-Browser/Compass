// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Space colors as the sidebar page handles them: compass::SpaceColor over
// mojo (null for grey), the palette, and conversions.

import type {SpaceColor} from './sidebar.mojom-webui.js';

// The space menu's palette, as SidebarTint (sidebar_tint.cc) tints the region
// with it (compass::SpaceColor's constants). Grey (null) isn't tinted. Any
// other color can be picked too (docs/design/spaces.md, "Space colors").
export interface PaletteColor {
  color: SpaceColor|null;
  name: string;
}

export const SPACE_COLORS: PaletteColor[] = [
  {color: null, name: 'Grey'},
  {color: {red: 59, green: 130, blue: 246}, name: 'Blue'},
  {color: {red: 139, green: 92, blue: 246}, name: 'Purple'},
  {color: {red: 236, green: 72, blue: 153}, name: 'Pink'},
  {color: {red: 239, green: 68, blue: 68}, name: 'Red'},
  {color: {red: 249, green: 115, blue: 22}, name: 'Orange'},
  {color: {red: 234, green: 179, blue: 8}, name: 'Yellow'},
  {color: {red: 34, green: 197, blue: 94}, name: 'Green'},
];


// A space's color and its gradient colors, at most (kMaxSpaceGradientColors).
export const MAX_GRADIENT_COLORS = 3;

// Hue in degrees (0-360), saturation and lightness (0-1).
export interface Hsl {
  hue: number;
  saturation: number;
  lightness: number;
}

export function colorToHsl(color: SpaceColor): Hsl {
  const r = color.red / 255;
  const g = color.green / 255;
  const b = color.blue / 255;
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const lightness = (max + min) / 2;
  const chroma = max - min;
  let hue = 0;
  if (chroma) {
    hue = max === r ? ((g - b) / chroma + 6) % 6 :
        max === g   ? (b - r) / chroma + 2 :
                      (r - g) / chroma + 4;
  }
  const saturation =
      chroma ? chroma / (1 - Math.abs(2 * lightness - 1)) : 0;
  return {hue: hue * 60, saturation: Math.min(saturation, 1), lightness};
}

export function hslToColor(hsl: Hsl): SpaceColor {
  const h = ((hsl.hue % 360) + 360) % 360;
  const s = Math.min(Math.max(hsl.saturation, 0), 1);
  const l = Math.min(Math.max(hsl.lightness, 0), 1);
  const c = (1 - Math.abs(2 * l - 1)) * s;
  const x = c * (1 - Math.abs((h / 60) % 2 - 1));
  const m = l - c / 2;
  const [r, g, b] = h < 60 ? [c, x, 0] :
      h < 120              ? [x, c, 0] :
      h < 180              ? [0, c, x] :
      h < 240              ? [0, x, c] :
      h < 300              ? [x, 0, c] :
                             [c, 0, x];
  const byte = (v: number) => Math.round((v + m) * 255);
  return {red: byte(r), green: byte(g), blue: byte(b)};
}

// |color| with its hue turned |degrees|.
export function rotateHue(color: SpaceColor, degrees: number): SpaceColor {
  const hsl = colorToHsl(color);
  return hslToColor({...hsl, hue: hsl.hue + degrees});
}

// The theme editor's second palette page: whole gradients.
export interface PaletteGradient {
  colors: SpaceColor[];
  name: string;
}

export const GRADIENT_PRESETS: PaletteGradient[] = [
  {name: 'Lagoon', colors: [{red: 34, green: 197, blue: 160},
                            {red: 56, green: 189, blue: 248}]},
  {name: 'Dusk', colors: [{red: 139, green: 92, blue: 246},
                          {red: 236, green: 72, blue: 153}]},
  {name: 'Ember', colors: [{red: 239, green: 68, blue: 68},
                           {red: 249, green: 115, blue: 22},
                           {red: 234, green: 179, blue: 8}]},
  {name: 'Meadow', colors: [{red: 132, green: 204, blue: 22},
                            {red: 34, green: 197, blue: 94}]},
  {name: 'Ocean', colors: [{red: 30, green: 64, blue: 175},
                           {red: 59, green: 130, blue: 246},
                           {red: 34, green: 211, blue: 238}]},
  {name: 'Peach', colors: [{red: 251, green: 146, blue: 60},
                           {red: 244, green: 114, blue: 182}]},
  {name: 'Aurora', colors: [{red: 34, green: 197, blue: 94},
                            {red: 6, green: 182, blue: 212},
                            {red: 168, green: 85, blue: 247}]},
  {name: 'Sand', colors: [{red: 214, green: 188, blue: 140},
                          {red: 190, green: 150, blue: 110}]},
  {name: 'Slate', colors: [{red: 71, green: 85, blue: 105},
                           {red: 100, green: 116, blue: 139}]},
];

// How often a custom color being dragged in the picker is passed on.
export const CUSTOM_COLOR_MS = 50;

export function sameColor(a: SpaceColor|null, b: SpaceColor|null): boolean {
  return a === b ||
      (!!a && !!b && a.red === b.red && a.green === b.green &&
       a.blue === b.blue);
}

export function colorToCss(color: SpaceColor|null): string {
  return color ? `rgb(${color.red}, ${color.green}, ${color.blue})` :
                 'rgb(128, 128, 128)';
}

// As <input type="color"> wants it.
export function colorToHex(color: SpaceColor|null): string {
  const c = color ?? {red: 128, green: 128, blue: 128};
  return '#' +
      [c.red, c.green, c.blue]
          .map(component => component.toString(16).padStart(2, '0'))
          .join('');
}

export function hexToColor(hex: string): SpaceColor|null {
  const match = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex);
  return match ? {
    red: parseInt(match[1]!, 16),
    green: parseInt(match[2]!, 16),
    blue: parseInt(match[3]!, 16),
  } :
                 null;
}
