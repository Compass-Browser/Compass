// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';

import {EmptyPageCallbackRouter, EmptyPageHandler} from './sidebar.mojom-webui.js';

// chrome://compass-empty: what a window with nothing else to show shows. There
// is no new tab page: the command bar opens whenever this page comes into view,
// and on a click. See docs/design/command-bar.md, "No new tab page".
const handler = EmptyPageHandler.getRemote();

// Follows the window's theme, which follows its space.
ColorChangeUpdater.forDocument().start();

// The space's tint over the frame color, as Arc paints an empty pane: the
// gradient corner to corner and the grain over it (the desktop's SidebarTint;
// docs/design/spaces.md, "Empty pane").
function cssColor(argb: number): string {
  const a = (argb >>> 24) & 0xff;
  const r = (argb >>> 16) & 0xff;
  const g = (argb >>> 8) & 0xff;
  const b = argb & 0xff;
  return `rgba(${r}, ${g}, ${b}, ${(a / 255).toFixed(3)})`;
}

// The same grey noise as the native tint's GrainTile(): a fixed linear
// congruential sequence, one grain per physical pixel.
const GRAIN_TILE_SIZE = 128;
function grainTile(): string {
  const canvas = document.createElement('canvas');
  canvas.width = canvas.height = GRAIN_TILE_SIZE;
  const context = canvas.getContext('2d')!;
  const image = context.createImageData(GRAIN_TILE_SIZE, GRAIN_TILE_SIZE);
  let state = 0x2545f491;
  for (let i = 0; i < GRAIN_TILE_SIZE * GRAIN_TILE_SIZE; ++i) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    const value = state >>> 24;
    image.data[i * 4] = image.data[i * 4 + 1] = image.data[i * 4 + 2] = value;
    image.data[i * 4 + 3] = 255;
  }
  context.putImageData(image, 0, 0);
  return canvas.toDataURL();
}

// The grain's opacity on this display, as the sidebar's
// (compass::GrainScaleMultiplier(), sidebar_tint.h): a grain is a physical
// pixel, coarser below 2x, so it's fainter there, down to half at 1x.
function grainScaleMultiplier(): number {
  return Math.min(1, Math.max(0.5, window.devicePixelRatio / 2));
}

const root = document.documentElement;
root.style.setProperty('--grain-tile', `url(${grainTile()})`);
// The space's grain opacity, before the display's multiplier.
let grainOpacity = 0;
let tinted = false;
function setGrainOpacity() {
  root.style.setProperty(
      '--grain-opacity',
      `${tinted ? grainOpacity * grainScaleMultiplier() : 0}`);
}
function setGrainSize() {
  root.style.setProperty(
      '--grain-size', `${GRAIN_TILE_SIZE / window.devicePixelRatio}px`);
  setGrainOpacity();
}
setGrainSize();
// Moving to a display with another scale resizes the page.
window.addEventListener('resize', setGrainSize);

const router = new EmptyPageCallbackRouter();
router.setTint.addListener((stops: number[], opacity: number) => {
  tinted = stops.some(stop => (stop >>> 24) !== 0);
  root.toggleAttribute('tinted', tinted);
  const colors = stops.map(cssColor).join(', ');
  root.style.setProperty(
      '--tint', `linear-gradient(to bottom right, ${colors})`);
  grainOpacity = opacity;
  setGrainOpacity();
});
handler.setPage(router.$.bindNewPipeAndPassRemote());

function showCommandBarIfVisible() {
  if (document.visibilityState === 'visible') {
    handler.showCommandBar();
  }
}

document.addEventListener('visibilitychange', showCommandBarIfVisible);
document.addEventListener('click', () => handler.showCommandBar());
showCommandBarIfVisible();
