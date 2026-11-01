// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {PaletteColor, PaletteGradient} from './colors.js';
import {SpaceAppearance} from './sidebar.mojom-webui.js';
import type {SpaceColor} from './sidebar.mojom-webui.js';
import type {CompassThemeEditorElement, DialTick, ModeOption} from './theme_editor.js';

export function getHtml(this: CompassThemeEditorElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div id="modes" role="radiogroup" aria-label="Appearance">
  ${this.modes_.map((mode: ModeOption) => html`
    <button class="mode" role="radio" data-appearance="${mode.appearance}"
        title="${mode.label}" aria-label="${mode.label}"
        aria-checked="${this.isMode_(mode)}" ?selected="${this.isMode_(mode)}"
        @click="${this.onModeClick_}">
      ${mode.appearance === SpaceAppearance.kAuto ? html`
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path d="M10 3l1.6 5.4L17 10l-5.4 1.6L10 17l-1.6-5.4L3 10l5.4-1.6z"></path>
          <path d="M18 14l.8 2.2L21 17l-2.2.8L18 20l-.8-2.2L15 17l2.2-.8z"></path>
        </svg>` : html`${mode.appearance === SpaceAppearance.kLight ? html`
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <circle cx="12" cy="12" r="4"></circle>
          <path class="rays" d="M12 2.5v2.5M12 19v2.5M2.5 12H5M19 12h2.5M5.3 5.3l1.8 1.8M16.9 16.9l1.8 1.8M5.3 18.7l1.8-1.8M16.9 7.1l1.8-1.8"></path>
        </svg>` : html`
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path d="M20 14.6A8.5 8.5 0 1 1 9.4 4a6.8 6.8 0 0 0 10.6 10.6z"></path>
        </svg>`}`}
    </button>`)}
</div>
<div id="pad" role="group" aria-label="Colors" style="${this.getPadStyle_()}"
    @pointerdown="${this.onPadPointerdown_}"
    @pointermove="${this.onPadPointermove_}"
    @pointerup="${this.onPadPointerup_}"
    @pointercancel="${this.onPadPointercancel_}">
  ${this.getColors_().map((color: SpaceColor, index: number) => html`
    <div class="dot" ?main="${index === 0}" data-index="${index}"
        role="slider" tabindex="0" aria-label="${this.getDotLabel_(index)}"
        aria-valuetext="${this.getHexLabel_(color)}"
        style="${this.getDotStyle_(color)}"
        @pointerdown="${this.onDotPointerdown_}"
        @keydown="${this.onDotKeydown_}">
    </div>`)}
  <div id="count" @pointerdown="${this.onCountPointerdown_}">
    <button id="removeColor" title="One color less"
        aria-label="One color less" ?disabled="${!this.canRemoveColor_()}"
        @click="${this.onRemoveColorClick_}">−</button>
    <button id="addColor" title="One more color" aria-label="One more color"
        ?disabled="${!this.canAddColor_()}"
        @click="${this.onAddColorClick_}">+</button>
  </div>
</div>
<div id="palette">
  <button class="page" title="Colors" aria-label="Colors"
      ?disabled="${this.isFirstPage_()}"
      @click="${this.onPreviousPageClick_}">‹</button>
  <div id="swatches" role="group" aria-label="Palette">
    ${this.isFirstPage_() ? html`${this.paletteColors_.map(
        (entry: PaletteColor, index: number) => html`
      <button class="swatch" data-index="${index}" title="${entry.name}"
          aria-label="${entry.name}"
          aria-pressed="${this.isPaletteColor_(entry)}"
          ?selected="${this.isPaletteColor_(entry)}"
          style="--swatch: ${this.getSwatchCss_(entry.color)}"
          @click="${this.onPaletteColorClick_}">
      </button>`)}` : html`
      ${this.paletteGradients_.map(
          (gradient: PaletteGradient, index: number) => html`
        <button class="swatch" data-index="${index}" title="${gradient.name}"
            aria-label="${gradient.name}"
            aria-pressed="${this.isPaletteGradient_(gradient)}"
            ?selected="${this.isPaletteGradient_(gradient)}"
            style="--swatch: ${this.getGradientCss_(gradient)}"
            @click="${this.onPaletteGradientClick_}">
        </button>`)}
      <label class="swatch" id="customColor" title="Any color">
        <input type="color" aria-label="Any color"
            .value="${this.getCustomHex_()}"
            @input="${this.onCustomColorInput_}">
      </label>`}
  </div>
  <button class="page" title="Gradients" aria-label="Gradients"
      ?disabled="${!this.isFirstPage_()}"
      @click="${this.onNextPageClick_}">›</button>
</div>
<div id="controls">
  <div id="wave" role="slider" tabindex="0" aria-label="Intensity"
      title="Intensity" aria-valuemin="0" aria-valuemax="100"
      aria-valuenow="${this.getIntensityPercent_()}"
      @pointerdown="${this.onWavePointerdown_}"
      @pointermove="${this.onWavePointermove_}"
      @pointerup="${this.onSliderPointerup_}"
      @pointercancel="${this.onSliderPointercancel_}"
      @keydown="${this.onWaveKeydown_}">
    <svg viewBox="0 0 200 36" preserveAspectRatio="none" aria-hidden="true">
      <path class="wave-line" d="${this.getWavePath_()}"></path>
      <path class="track-line" d="${this.getTrackPath_()}"></path>
    </svg>
    <div id="waveThumb" style="${this.getThumbStyle_()}"></div>
  </div>
  <div id="dial" role="slider" tabindex="0" aria-label="Grain" title="Grain"
      aria-valuemin="0" aria-valuemax="100"
      aria-valuenow="${this.getGrainPercent_()}"
      @pointerdown="${this.onDialPointerdown_}"
      @pointermove="${this.onDialPointermove_}"
      @pointerup="${this.onSliderPointerup_}"
      @pointercancel="${this.onSliderPointercancel_}"
      @keydown="${this.onDialKeydown_}">
    ${this.getDialTicks_().map((tick: DialTick) => html`
      <span class="tick" ?lit="${tick.lit}"
          style="${this.getTickStyle_(tick)}"></span>`)}
    <div id="knob" style="${this.getKnobStyle_()}"></div>
  </div>
</div>
<!--_html_template_end_-->`;
  // clang-format on
}
