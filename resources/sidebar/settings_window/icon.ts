// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The settings window's icons: line glyphs on a 24-unit grid, drawn with
// currentColor at a 1.6 stroke (the toolbar's and the rows'), in the manner
// of the system's symbols rather than Material's filled icons.

export const ICON_VIEW_BOX = '0 0 24 24';

// Each icon is a list of SVG path data, stroked.
export const ICONS: Record<string, string[]> = {
  general: [
    'M12 8.6a3.4 3.4 0 1 0 0 6.8a3.4 3.4 0 1 0 0-6.8z',
    'M19.4 13.5a7.6 7.6 0 0 0 0-3l2-1.6-2-3.4-2.4 1a7.5 7.5 0 0 0-2.6-1.5' +
        'L14 2.5h-4l-.4 2.5A7.5 7.5 0 0 0 7 6.5l-2.4-1-2 3.4 2 1.6a7.6 7.6 0' +
        ' 0 0 0 3l-2 1.6 2 3.4 2.4-1a7.5 7.5 0 0 0 2.6 1.5l.4 2.5h4l.4-2.5' +
        'a7.5 7.5 0 0 0 2.6-1.5l2.4 1 2-3.4z',
  ],
  profiles: [
    'M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z',
    'M12 7.5a3 3 0 1 0 0 6a3 3 0 1 0 0-6z',
    'M6.2 18.2c1.3-1.9 3.4-3 5.8-3s4.5 1.1 5.8 3',
  ],
  links: [
    'M10 14a4.2 4.2 0 0 0 6 0l3-3a4.2 4.2 0 0 0-6-6l-1 1',
    'M14 10a4.2 4.2 0 0 0-6 0l-3 3a4.2 4.2 0 0 0 6 6l1-1',
  ],
  shortcuts: [
    'M9 9h6v6H9z',
    'M9 9V6.5A2.5 2.5 0 1 0 6.5 9H9z',
    'M15 9V6.5A2.5 2.5 0 1 1 17.5 9H15z',
    'M9 15v2.5A2.5 2.5 0 1 1 6.5 15H9z',
    'M15 15v2.5a2.5 2.5 0 1 0 2.5-2.5H15z',
  ],
  boosts: [
    'M14.5 4.5l5 5L10 19l-5.5.5L5 14z',
    'M12.5 6.5l5 5',
  ],
  plugins: [
    'M9 4.5a2 2 0 0 1 4 0V6h4v4h1.5a2 2 0 0 1 0 4H17v5H6v-4h1.5a2 2 0 0' +
        ' 0 0-4H6V6h3z',
  ],
  ai: [
    'M12 3.5c.6 4.2 2.3 5.9 6.5 6.5-4.2.6-5.9 2.3-6.5 6.5-.6-4.2-2.3-5.9' +
        '-6.5-6.5 4.2-.6 5.9-2.3 6.5-6.5z',
    'M18.5 15.5c.3 1.8 1 2.6 2.8 2.8-1.8.3-2.5 1-2.8 2.8-.3-1.8-1-2.5-2.8' +
        '-2.8 1.8-.2 2.5-1 2.8-2.8z',
  ],
  privacy: [
    'M12 3l7 2.8v5.4c0 4.5-3 8.2-7 9.8-4-1.6-7-5.3-7-9.8V5.8z',
    'M9 12l2 2 4-4',
  ],
  passwords: [
    'M8.5 10.5a4 4 0 1 0 0 8a4 4 0 1 0 0-8z',
    'M11.4 11.6L19 4',
    'M16 7l2.5 2.5',
    'M14 9l1.5 1.5',
  ],
  advanced: [
    'M4 7h9', 'M17 7h3', 'M15 5v4',
    'M4 17h3', 'M11 17h9', 'M9 15v4',
    'M4 12h13', 'M20 12h0',
  ],
  search: [
    'M10.5 4.5a6 6 0 1 0 0 12a6 6 0 1 0 0-12z',
    'M15 15l5 5',
  ],
  external: [
    'M14 4h6v6', 'M20 4l-9 9', 'M18 14v5H5V6h5',
  ],
  chevron: ['M9.5 6l6 6-6 6'],
  back: ['M14.5 6l-6 6 6 6'],
};

const SVG_NS = 'http://www.w3.org/2000/svg';

// Its size and stroke come from the page: --icon-size, --icon-stroke.
const ICON_STYLE = `
:host {
  display: inline-flex;
  flex: none;
}

svg {
  fill: none;
  height: var(--icon-size, 20px);
  stroke: currentColor;
  stroke-linecap: round;
  stroke-linejoin: round;
  stroke-width: var(--icon-stroke, 1.6);
  width: var(--icon-size, 20px);
}
`;

// <compass-settings-icon name="general">: one of ICONS, as an inline SVG.
// Drawn in a shadow root: children of its own would shift the nodes of a Lit
// template being cloned around it, and Lit would put its parts in the wrong
// places.
export class CompassSettingsIconElement extends HTMLElement {
  static get observedAttributes() {
    return ['name'];
  }

  private svg_: SVGSVGElement;

  constructor() {
    super();
    const root = this.attachShadow({mode: 'open'});
    const style = document.createElement('style');
    style.textContent = ICON_STYLE;
    this.svg_ = document.createElementNS(SVG_NS, 'svg');
    this.svg_.setAttribute('viewBox', ICON_VIEW_BOX);
    this.svg_.setAttribute('aria-hidden', 'true');
    root.append(style, this.svg_);
  }

  attributeChangedCallback() {
    const paths = (ICONS[this.getAttribute('name') || ''] || []).map(d => {
      const path = document.createElementNS(SVG_NS, 'path');
      path.setAttribute('d', d);
      return path;
    });
    this.svg_.replaceChildren(...paths);
  }
}

customElements.define('compass-settings-icon', CompassSettingsIconElement);
