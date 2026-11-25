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
  spaces: [
    'M4 6.5A2.5 2.5 0 0 1 6.5 4h7A2.5 2.5 0 0 1 16 6.5v7a2.5 2.5 0 0 1-2.5' +
        ' 2.5h-7A2.5 2.5 0 0 1 4 13.5z',
    'M8 20h9.5a2.5 2.5 0 0 0 2.5-2.5V8',
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
  about: ['M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z', 'M12 11v5', 'M12 8h0'],

  // Row tiles' glyphs (.tile, drawn white on a colored square).
  sync: ['M4 12a8 8 0 0 1 14-5.3', 'M20 12a8 8 0 0 1-14 5.3', 'M18 3v4h-4',
         'M6 21v-4h4'],
  import: ['M12 4v11', 'M7.5 10.5L12 15l4.5-4.5',
           'M4 15v3.5A1.5 1.5 0 0 0 5.5 20h13a1.5 1.5 0 0 0 1.5-1.5V15'],
  download: ['M12 4v11', 'M7.5 10.5L12 15l4.5-4.5', 'M5 19h14'],
  update: ['M12 4v10', 'M8 10l4 4 4-4', 'M5 19h14'],
  folder: ['M3.5 7.5A1.5 1.5 0 0 1 5 6h4l2 2h8a1.5 1.5 0 0 1 1.5 1.5v8A1.5' +
           ' 1.5 0 0 1 19 19H5a1.5 1.5 0 0 1-1.5-1.5z'],
  bookmark: ['M7 4h10v16l-5-3.5L7 20z'],
  plug: ['M9 3v5', 'M15 3v5', 'M6.5 8h11v3a5.5 5.5 0 0 1-11 0z',
         'M12 16.5V21'],
  signin: ['M10 7l5 5-5 5', 'M15 12H4',
           'M14 4h4.5A1.5 1.5 0 0 1 20 5.5v13a1.5 1.5 0 0 1-1.5 1.5H14'],
  lock: ['M6 11h12v9H6z', 'M8.5 11V8a3.5 3.5 0 0 1 7 0v3'],
  card: ['M3.5 6.5h17v11h-17z', 'M3.5 10h17', 'M7 14.5h3'],
  home: ['M4 11l8-6.5 8 6.5', 'M6 9.5V19h12V9.5'],
  compass: ['M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z', 'M15.5 8.5l-2 5-5 2 2-5z'],
  globe: ['M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z', 'M3 12h18',
          'M12 3c2.5 2.5 3.8 5.5 3.8 9s-1.3 6.5-3.8 9c-2.5-2.5-3.8-5.5-3.8-9' +
              'S9.5 5.5 12 3z'],
  pip: ['M3.5 5.5h17v13h-17z', 'M12 12h6.5v4.5H12z'],
  textsize: ['M4 18l5-12 5 12', 'M5.8 14h6.4', 'M15 18l3-7 3 7',
             'M15.9 16h4.2'],
  zoom: ['M10.5 4.5a6 6 0 1 0 0 12a6 6 0 1 0 0-12z', 'M15 15l5 5',
         'M10.5 8v5', 'M8 10.5h5'],
  font: ['M6 19L12 5l6 14', 'M8.5 14h7'],
  translate: ['M4 6h9', 'M8.5 4v2', 'M6 6c.8 3 2.8 5.5 5.5 7',
              'M11 6c-.8 3-2.8 5.5-5.5 7', 'M13 20l3.5-8 3.5 8', 'M14 17.5h5'],
  spell: ['M4 14l3-8 3 8', 'M5 11.5h4', 'M13 15l3 3 5-7'],
  chip: ['M7 7h10v10H7z', 'M10 3v4', 'M14 3v4', 'M10 17v4', 'M14 17v4',
         'M3 10h4', 'M3 14h4', 'M17 10h4', 'M17 14h4'],
  network: ['M12 4v4', 'M5 20v-3h14v3', 'M12 8a2 2 0 1 0 0 4a2 2 0 1 0 0-4z',
            'M12 12v5'],
  leaf: ['M5 19c0-8 5-13 14-14 0 9-5 14-13 14z', 'M5 19l7-7'],
  bolt: ['M13 3L6 13h5l-1 8 7-10h-5z'],
  caption: ['M3.5 6h17v12h-17z', 'M7 11h4', 'M7 14h7', 'M14 11h3'],
  focus: ['M4 8V4h4', 'M16 4h4v4', 'M20 16v4h-4', 'M8 20H4v-4'],
  cursor: ['M9 4h6', 'M9 20h6', 'M12 4v16'],
  reset: ['M4 12a8 8 0 1 0 2.3-5.7', 'M4 4v4h4'],
  doc: ['M7 3.5h7l4 4V20.5H7z', 'M14 3.5v4h4', 'M9.5 12h6', 'M9.5 15.5h6'],
  pulse: ['M4 12h3l2-6 3 12 2-6h6'],
  code: ['M8 8l-4 4 4 4', 'M16 8l4 4-4 4', 'M13.5 5l-3 14'],
  sidebar: ['M4 5h16v14H4z', 'M9 5v14'],
  server: ['M4 5h16v6H4z', 'M4 13h16v6H4z', 'M8 8h0', 'M8 16h0'],
  trash: ['M5 7h14', 'M10 7V4.5h4V7', 'M7 7l1 12.5h8L17 7'],
  cookie: [
    'M12 3a9 9 0 1 0 9 9 3 3 0 0 1-3.5-3A3 3 0 0 1 14 5.5 2.5 2.5 0 0 1 12 3z',
    'M9 10h0', 'M14.5 14.5h0', 'M9.5 15.5h0',
  ],
  eyeoff: ['M3 12s3.5-6 9-6 9 6 9 6-3.5 6-9 6-9-6-9-6z',
           'M12 9.5a2.5 2.5 0 1 0 0 5a2.5 2.5 0 1 0 0-5z', 'M4 4l16 16'],
  filter: ['M4 6h16', 'M7 12h10', 'M10 18h4'],
  check: ['M6 12.5l4 4 8-9'],
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
