// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from '//resources/lit/v3_0/lit.rollup.js';

import {SidebarBrowserProxy} from './browser_proxy.js';
import {colorToCss, colorToHex, colorToHsl, CUSTOM_COLOR_MS, GRADIENT_PRESETS, hexToColor, hslToColor, MAX_GRADIENT_COLORS, rotateHue, sameColor, SPACE_COLORS} from './colors.js';
import type {PaletteColor, PaletteGradient} from './colors.js';
import {SpaceAppearance} from './sidebar.mojom-webui.js';
import type {Space, SpaceColor} from './sidebar.mojom-webui.js';
import {getCss} from './theme_editor.css.js';
import {getHtml} from './theme_editor.html.js';

// How far from the pad's center (as a fraction of its half width and half
// height) a fully saturated color sits.
const PAD_REACH = 0.42;
// A new color's lightness, and the colors' lightness when the pad sets one
// from nothing (a grey space).
const DEFAULT_LIGHTNESS = 0.55;
// The intensity dial's ticks, and how far it turns, in degrees each way.
const DIAL_TICKS = 21;
const DIAL_TURN = 135;
// Pixels of drag for the dial's whole range.
const DIAL_DRAG_RANGE = 140;
// After letting go, how long what was dragged shows over a space the browser
// hasn't caught up with yet.
const SETTLE_MS = 600;

export interface ModeOption {
  appearance: SpaceAppearance;
  label: string;
}

const MODES: ModeOption[] = [
  {appearance: SpaceAppearance.kAuto, label: 'Match the system'},
  {appearance: SpaceAppearance.kLight, label: 'Light'},
  {appearance: SpaceAppearance.kDark, label: 'Dark'},
];

export interface DialTick {
  angle: number;
  lit: boolean;
}

// Edits a space's theme (docs/design/spaces.md, "Theme editor"): light or
// dark mode, its colors as dots on a hue and saturation pad (the big one is
// its color, the small ones its gradient), a palette of colors and
// gradients, the tint's intensity (the wave) and its grain (the dial).
// Changes go to the browser as they're made; dragging sends one every
// CUSTOM_COLOR_MS.
export class CompassThemeEditorElement extends CrLitElement {
  static get is() {
    return 'compass-theme-editor';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      space: {type: Object},
      page_: {type: Number},
      dragColors_: {type: Array},
      dragIntensity_: {type: Number},
      dragGrain_: {type: Number},
    };
  }

  accessor space: Space|null = null;
  // The palette's page: colors, then gradients.
  protected accessor page_: number = 0;
  // While dragging and just after: what's shown instead of the space's.
  protected accessor dragColors_: SpaceColor[]|null = null;
  protected accessor dragIntensity_: number|null = null;
  protected accessor dragGrain_: number|null = null;

  protected readonly modes_: ModeOption[] = MODES;
  protected readonly paletteColors_: PaletteColor[] = SPACE_COLORS;
  protected readonly paletteGradients_: PaletteGradient[] =
      GRADIENT_PRESETS.slice(0, 8);

  private proxy_ = SidebarBrowserProxy.getInstance();
  // The dot being dragged, and the colors when the drag began.
  private dragIndex_: number|null = null;
  private dragStart_: SpaceColor[] = [];
  private dialStart_: {value: number, x: number, y: number}|null = null;
  private pendingSend_: (() => void)|null = null;
  private sendTimer_: number|null = null;
  private settleTimer_: number|null = null;

  override willUpdate(changed: PropertyValues<this>) {
    super.willUpdate(changed);
    // The browser caught up with what was dragged.
    if (changed.has('space') && this.dragIndex_ === null &&
        this.dialStart_ === null && this.matchesDrag_()) {
      this.clearDrag_();
    }
  }

  // The space's colors: its color, then its gradient; none for grey.
  protected getColors_(): SpaceColor[] {
    if (this.dragColors_) {
      return this.dragColors_;
    }
    const space = this.space;
    return space?.color ? [space.color, ...space.gradient] : [];
  }

  protected getIntensity_(): number {
    return this.dragIntensity_ ?? this.space?.intensity ?? 0;
  }

  protected getGrain_(): number {
    return this.dragGrain_ ?? this.space?.grain ?? 0;
  }

  protected isMode_(mode: ModeOption): boolean {
    return (this.space?.appearance ?? SpaceAppearance.kAuto) ===
        mode.appearance;
  }

  protected onModeClick_(e: Event) {
    const appearance = Number((e.currentTarget as HTMLElement)
                                  .dataset['appearance']) as SpaceAppearance;
    if (this.space) {
      this.proxy_.handler.setSpaceAppearance(this.space.id, appearance);
    }
  }

  // The pad's backdrop, tinted with the space's colors.
  protected getPadStyle_(): string {
    const colors = this.getColors_();
    const first = colorToCss(colors[0] ?? null);
    const last = colorToCss(colors[colors.length - 1] ?? null);
    return `--pad-color-1: ${first}; --pad-color-2: ${last}`;
  }

  // Where a color sits on the pad: its hue as the angle (red at the top,
  // going clockwise), its saturation as the distance from the center.
  protected getDotStyle_(color: SpaceColor): string {
    const {hue, saturation} = colorToHsl(color);
    const angle = hue * Math.PI / 180;
    const x = 50 + Math.sin(angle) * saturation * PAD_REACH * 100;
    const y = 50 - Math.cos(angle) * saturation * PAD_REACH * 100;
    return `left: ${x}%; top: ${y}%; --dot-color: ${colorToCss(color)}`;
  }

  protected getDotLabel_(index: number): string {
    return index === 0 ? 'Main color' : `Gradient color ${index + 1}`;
  }

  protected getHexLabel_(color: SpaceColor): string {
    return colorToHex(color);
  }

  // A dot follows the pointer. The big one takes the others along, so the
  // gradient keeps its shape.
  protected onDotPointerdown_(e: PointerEvent) {
    e.stopPropagation();
    const dot = e.currentTarget as HTMLElement;
    this.startDrag_(Number(dot.dataset['index']), e);
  }

  // A click on the pad moves the big dot there (or makes the first color).
  protected onPadPointerdown_(e: PointerEvent) {
    if (e.button !== 0) {
      return;
    }
    this.startDrag_(0, e);
    this.onPadPointermove_(e);
  }

  protected onPadPointermove_(e: PointerEvent) {
    if (this.dragIndex_ === null) {
      return;
    }
    const pad = this.shadowRoot.querySelector<HTMLElement>('#pad')!;
    const box = pad.getBoundingClientRect();
    const dx = (e.clientX - box.left - box.width / 2) / (box.width / 2) /
        PAD_REACH;
    const dy = (e.clientY - box.top - box.height / 2) / (box.height / 2) /
        PAD_REACH;
    const saturation = Math.min(1, Math.hypot(dx, dy));
    const hue = (Math.atan2(dx, -dy) * 180 / Math.PI + 360) % 360;
    this.moveDot_(this.dragIndex_, hue, saturation);
  }

  protected onPadPointerup_() {
    if (this.dragIndex_ === null) {
      return;
    }
    this.dragIndex_ = null;
    this.flushSend_();
    this.settleSoon_();
  }

  protected onPadPointercancel_() {
    this.onPadPointerup_();
  }

  protected onDotKeydown_(e: KeyboardEvent) {
    const index = Number((e.currentTarget as HTMLElement).dataset['index']);
    const color = this.getColors_()[index];
    if (!color) {
      return;
    }
    const {hue, saturation} = colorToHsl(color);
    const steps: Record<string, [number, number]> = {
      ArrowLeft: [-5, 0],
      ArrowRight: [5, 0],
      ArrowUp: [0, 0.05],
      ArrowDown: [0, -0.05],
    };
    const step = steps[e.key];
    if (!step) {
      return;
    }
    e.preventDefault();
    this.dragStart_ = this.getColors_();
    this.moveDot_(index, hue + step[0],
                  Math.min(1, Math.max(0, saturation + step[1])));
    this.flushSend_();
    this.settleSoon_();
  }

  private startDrag_(index: number, e: PointerEvent) {
    const pad = this.shadowRoot.querySelector<HTMLElement>('#pad')!;
    pad.setPointerCapture(e.pointerId);
    this.dragIndex_ = index;
    this.dragStart_ = this.getColors_();
    e.preventDefault();
  }

  // Moves dot |index| to |hue| and |saturation|, keeping its lightness.
  private moveDot_(index: number, hue: number, saturation: number) {
    const start = this.dragStart_;
    let colors: SpaceColor[];
    if (start.length === 0) {
      colors =
          [hslToColor({hue, saturation, lightness: DEFAULT_LIGHTNESS})];
    } else {
      const from = colorToHsl(start[index]!);
      const moved = hslToColor({hue, saturation, lightness: from.lightness});
      colors = [...start];
      colors[index] = moved;
      if (index === 0) {
        const turn = hue - from.hue;
        const push = saturation - from.saturation;
        for (let i = 1; i < start.length; ++i) {
          const other = colorToHsl(start[i]!);
          colors[i] = hslToColor({
            hue: other.hue + turn,
            saturation: Math.min(1, Math.max(0, other.saturation + push)),
            lightness: other.lightness,
          });
        }
      }
    }
    this.dragColors_ = colors;
    this.sendSoon_(() => this.sendColors_(colors));
  }

  // − and + are buttons, not places on the pad.
  protected onCountPointerdown_(e: Event) {
    e.stopPropagation();
  }

  protected canRemoveColor_(): boolean {
    return this.getColors_().length > 1;
  }

  protected canAddColor_(): boolean {
    const count = this.getColors_().length;
    return count > 0 && count < MAX_GRADIENT_COLORS;
  }

  protected onRemoveColorClick_() {
    const colors = this.getColors_();
    if (colors.length > 1) {
      this.sendColors_(colors.slice(0, -1));
    }
  }

  // The new color goes on from the last one, a little further round the hue
  // wheel.
  protected onAddColorClick_() {
    const colors = this.getColors_();
    if (this.canAddColor_()) {
      this.sendColors_([...colors, rotateHue(colors[colors.length - 1]!, 40)]);
    }
  }

  protected isFirstPage_(): boolean {
    return this.page_ === 0;
  }

  protected onPreviousPageClick_() {
    this.page_ = 0;
  }

  protected onNextPageClick_() {
    this.page_ = 1;
  }

  protected getSwatchCss_(color: SpaceColor|null): string {
    return colorToCss(color);
  }

  protected getGradientCss_(gradient: PaletteGradient): string {
    return `linear-gradient(135deg, ${
        gradient.colors.map(color => colorToCss(color)).join(', ')})`;
  }

  protected isPaletteColor_(entry: PaletteColor): boolean {
    const colors = this.getColors_();
    return entry.color ? colors.length === 1 && sameColor(colors[0]!, entry.color) :
                         colors.length === 0;
  }

  protected isPaletteGradient_(gradient: PaletteGradient): boolean {
    const colors = this.getColors_();
    return colors.length === gradient.colors.length &&
        colors.every((color, i) => sameColor(color, gradient.colors[i]!));
  }

  // A palette color replaces the gradient too.
  protected onPaletteColorClick_(e: Event) {
    const entry = SPACE_COLORS[Number(
        (e.currentTarget as HTMLElement).dataset['index'])];
    if (entry) {
      this.sendColors_(entry.color ? [entry.color] : []);
    }
  }

  protected onPaletteGradientClick_(e: Event) {
    const gradient = GRADIENT_PRESETS[Number(
        (e.currentTarget as HTMLElement).dataset['index'])];
    if (gradient) {
      this.sendColors_(gradient.colors);
    }
  }

  protected getCustomHex_(): string {
    return colorToHex(this.getColors_()[0] ?? null);
  }

  // The color picker sets the main color only.
  protected onCustomColorInput_(e: Event) {
    const color = hexToColor((e.target as HTMLInputElement).value);
    if (!color) {
      return;
    }
    const colors = this.getColors_();
    const next = colors.length ? [color, ...colors.slice(1)] : [color];
    this.dragColors_ = next;
    this.sendSoon_(() => this.sendColors_(next));
    this.settleSoon_();
  }

  // The wave grows with the intensity, up to the thumb; flat after it.
  protected getWavePath_(): string {
    const value = this.getIntensity_();
    const end = value * 200;
    const amplitude = 2 + value * 9;
    let path = 'M 0 18';
    for (let x = 2; x <= end; x += 2) {
      path += ` L ${x} ${18 - amplitude * Math.sin(x * Math.PI / 12)}`;
    }
    return path;
  }

  protected getTrackPath_(): string {
    return `M ${this.getIntensity_() * 200} 18 L 200 18`;
  }

  protected getThumbStyle_(): string {
    return `left: ${this.getIntensity_() * 100}%`;
  }

  protected getIntensityPercent_(): number {
    return Math.round(this.getIntensity_() * 100);
  }

  protected onWavePointerdown_(e: PointerEvent) {
    if (e.button !== 0) {
      return;
    }
    const wave = e.currentTarget as HTMLElement;
    wave.setPointerCapture(e.pointerId);
    this.dialStart_ = {value: this.getIntensity_(), x: 0, y: 0};
    this.onWavePointermove_(e);
  }

  protected onWavePointermove_(e: PointerEvent) {
    if (!this.dialStart_) {
      return;
    }
    const box = (e.currentTarget as HTMLElement).getBoundingClientRect();
    this.setIntensity_((e.clientX - box.left) / box.width);
  }

  protected onSliderPointerup_() {
    this.dialStart_ = null;
    this.flushSend_();
    this.settleSoon_();
  }

  protected onSliderPointercancel_() {
    this.onSliderPointerup_();
  }

  protected onWaveKeydown_(e: KeyboardEvent) {
    const value = this.stepValue_(this.getIntensity_(), e);
    if (value !== null) {
      this.setIntensity_(value);
      this.flushSend_();
      this.settleSoon_();
    }
  }

  private setIntensity_(value: number) {
    const intensity = Math.min(1, Math.max(0, value));
    this.dragIntensity_ = intensity;
    const id = this.space?.id;
    if (id) {
      this.sendSoon_(
          () => this.proxy_.handler.setSpaceIntensity(id, intensity));
    }
  }

  protected getDialTicks_(): DialTick[] {
    const lit = Math.round(this.getGrain_() * (DIAL_TICKS - 1));
    return Array.from({length: DIAL_TICKS}, (_, i) => ({
                                              angle: -DIAL_TURN +
                                                  2 * DIAL_TURN * i /
                                                      (DIAL_TICKS - 1),
                                              lit: i <= lit && lit > 0,
                                            }));
  }

  protected getTickStyle_(tick: DialTick): string {
    return `transform: rotate(${tick.angle}deg) translateY(-25px)`;
  }

  protected getKnobStyle_(): string {
    return `transform: rotate(${
        -DIAL_TURN + 2 * DIAL_TURN * this.getGrain_()}deg)`;
  }

  protected getGrainPercent_(): number {
    return Math.round(this.getGrain_() * 100);
  }

  // Up or right turns it up.
  protected onDialPointerdown_(e: PointerEvent) {
    if (e.button !== 0) {
      return;
    }
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    this.dialStart_ = {value: this.getGrain_(), x: e.clientX, y: e.clientY};
  }

  protected onDialPointermove_(e: PointerEvent) {
    const start = this.dialStart_;
    if (!start) {
      return;
    }
    this.setGrain_(start.value +
                   ((e.clientX - start.x) - (e.clientY - start.y)) /
                       DIAL_DRAG_RANGE);
  }

  protected onDialKeydown_(e: KeyboardEvent) {
    const value = this.stepValue_(this.getGrain_(), e);
    if (value !== null) {
      this.setGrain_(value);
      this.flushSend_();
      this.settleSoon_();
    }
  }

  private setGrain_(value: number) {
    const grain = Math.min(1, Math.max(0, value));
    this.dragGrain_ = grain;
    const id = this.space?.id;
    if (id) {
      this.sendSoon_(() => this.proxy_.handler.setSpaceGrain(id, grain));
    }
  }

  // A slider's keys: arrows by 2%, Page Up/Down by 10%, Home and End.
  private stepValue_(value: number, e: KeyboardEvent): number|null {
    const steps: Record<string, number> = {
      ArrowLeft: -0.02,
      ArrowDown: -0.02,
      ArrowRight: 0.02,
      ArrowUp: 0.02,
      PageDown: -0.1,
      PageUp: 0.1,
      Home: -1,
      End: 1,
    };
    const step = steps[e.key];
    if (step === undefined) {
      return null;
    }
    e.preventDefault();
    return Math.min(1, Math.max(0, value + step));
  }

  // The space's color and gradient as |colors| (none for grey). The color
  // goes first: a grey space ignores a gradient.
  private sendColors_(colors: SpaceColor[]) {
    const id = this.space?.id;
    if (!id) {
      return;
    }
    this.proxy_.handler.setSpaceColor(id, colors[0] ?? null);
    this.proxy_.handler.setSpaceGradient(id, colors.slice(1));
  }

  private sendSoon_(send: () => void) {
    this.pendingSend_ = send;
    if (this.sendTimer_ !== null) {
      return;
    }
    this.sendTimer_ = setTimeout(() => {
      this.sendTimer_ = null;
      this.flushSend_();
    }, CUSTOM_COLOR_MS);
  }

  private flushSend_() {
    const send = this.pendingSend_;
    this.pendingSend_ = null;
    send?.();
  }

  private settleSoon_() {
    if (this.settleTimer_ !== null) {
      clearTimeout(this.settleTimer_);
    }
    this.settleTimer_ = setTimeout(() => {
      this.settleTimer_ = null;
      if (this.dragIndex_ === null && this.dialStart_ === null) {
        this.clearDrag_();
      }
    }, SETTLE_MS);
  }

  private matchesDrag_(): boolean {
    const space = this.space;
    if (!space) {
      return true;
    }
    const colors = space.color ? [space.color, ...space.gradient] : [];
    const drag = this.dragColors_;
    return (!drag ||
            (drag.length === colors.length &&
             drag.every((color, i) => sameColor(color, colors[i]!)))) &&
        (this.dragIntensity_ === null ||
         Math.abs(this.dragIntensity_ - space.intensity) < 1e-6) &&
        (this.dragGrain_ === null ||
         Math.abs(this.dragGrain_ - space.grain) < 1e-6);
  }

  private clearDrag_() {
    this.dragColors_ = null;
    this.dragIntensity_ = null;
    this.dragGrain_ = null;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-theme-editor': CompassThemeEditorElement;
  }
}

customElements.define(
    CompassThemeEditorElement.is, CompassThemeEditorElement);
