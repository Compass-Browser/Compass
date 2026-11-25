// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The settings window's panes, in the toolbar's order. A pane shows its
// sections (the window's own elements, and Compass's settings/*_page.ts) and
// groups of rows (docs/design/settings-window.md).

// A row inside a section's element that search can take you to (the
// element marks it with data-row).
export interface SearchRow {
  id: string;
  label: string;
  keywords?: string;
  // The path that shows it, when it's in one of the section's views
  // ("general?view=fonts"); the section's own path otherwise.
  path?: string;
}

// The colors of the row tiles and pane headers (window.css, .tile).
export type TileColor = 'blue'|'indigo'|'purple'|'pink'|'red'|'orange'|
    'yellow'|'green'|'teal'|'gray'|'slate';

// A section shown on a page of its own: the pane lists it as a row (with the
// status the element reports, the `subpage-detail` event), and its path
// opens it, with a way back to the pane.
export interface Subpage {
  // The caption of the group its row is in.
  group: string;
  label: string;
  icon: string;
  color: TileColor;
}

// One of Compass's sections, or a pane rebuilt from upstream's, shown in a
// pane.
export interface PaneSection {
  // Its element.
  tag: string;
  // The path that shows it ("sync": the Profiles pane, scrolled to it).
  path: string;
  // Words search finds it by, besides its text.
  keywords: string;
  // Its rows search lists on their own.
  searchRows?: SearchRow[];
  // Shown on a page of its own, from a row in the pane.
  subpage?: Subpage;
}

// A row that opens a browser page at |url| in a tab.
export interface LinkRow {
  id: string;
  label: string;
  detail: string;
  url?: string;
  keywords?: string;
}

export interface RowGroup {
  title: string;
  rows: LinkRow[];
}

export interface Pane {
  id: string;
  label: string;
  // The pane's header: a tile in its color, its label and what it's for. A
  // pane without a blurb draws its own heading (About).
  color: TileColor;
  blurb: string;
  sections: PaneSection[];
  groups: RowGroup[];
}

export const PANES: Pane[] = [
  {
    id: 'general',
    label: 'General',
    color: 'gray',
    blurb: 'How Compass opens, searches, downloads and shows pages.',
    sections: [{
      tag: 'compass-settings-general-pane',
      path: 'general',
      keywords: '',
      searchRows: [
        {id: 'defaultBrowser', label: 'Default browser'},
        {
          id: 'search',
          label: 'Search engine',
          keywords: 'google bing duckduckgo omnibox',
        },
        {id: 'searchEngines', label: 'Manage search engines and site search'},
        {id: 'downloads', label: 'Download location', keywords: 'folder'},
        {id: 'promptForDownload', label: 'Ask where to save each file'},
        {id: 'fontSize', label: 'Font size', keywords: 'text larger smaller'},
        {id: 'zoom', label: 'Page zoom'},
        {id: 'webkit.webprefs.fonts.standard.Zyyy', label: 'Fonts',
         keywords: 'serif sans-serif fixed typeface',
         path: 'general?view=fonts'},
        {id: 'minimumFontSize', label: 'Minimum font size',
         path: 'general?view=fonts'},
        {id: 'languages', label: 'Preferred languages'},
        {id: 'translate', label: 'Offer to translate pages'},
        {id: 'spellcheck', label: 'Check spelling as you type',
         keywords: 'spellcheck'},
        {id: 'hardwareAcceleration',
         label: 'Use graphics acceleration when available',
         keywords: 'hardware gpu'},
        {id: 'proxy', label: 'Proxy settings', keywords: 'network'},
        {id: 'pipOnLeavingWindow',
         label: 'Picture-in-picture when you leave the window',
         keywords: 'video call pip'},
      ],
    }],
    groups: [],
  },
  {
    id: 'profiles',
    label: 'Profiles',
    color: 'purple',
    blurb: 'Separate people and accounts, each with their own spaces, sync ' +
        'and passwords.',
    sections: [
      {
        tag: 'compass-settings-profile-section',
        path: 'profiles',
        keywords: '',
        searchRows: [
          {id: 'profileName', label: 'Profile name', keywords: 'people'},
          {
            id: 'addProfile',
            label: 'New profile',
            keywords: 'add create people',
          },
        ],
      },
      {
        tag: 'compass-sync-page',
        path: 'sync',
        keywords: 'sync devices account sign in pair',
        subpage: {
          group: 'Sync and import',
          label: 'Sync',
          icon: 'sync',
          color: 'blue',
        },
      },
      {
        tag: 'compass-arc-import-page',
        path: 'importArc',
        keywords: 'import arc chrome safari firefox bookmarks history',
        subpage: {
          group: 'Sync and import',
          label: 'Import from another browser',
          icon: 'import',
          color: 'green',
        },
      },
    ],
    groups: [],
  },
  {
    id: 'spaces',
    label: 'Spaces',
    color: 'indigo',
    blurb:
        'Your spaces: their names, icons, themes and the profile each is in.',
    sections: [{
      tag: 'compass-settings-spaces-pane',
      path: 'spaces',
      keywords: 'space spaces workspace switcher icon emoji color theme ' +
          'gradient profile archive sync rename reorder delete',
      searchRows: [
        {id: 'spaces', label: 'Edit spaces', keywords: 'rename icon theme'},
      ],
    }],
    groups: [],
  },
  {
    id: 'links',
    label: 'Links',
    color: 'teal',
    blurb: 'Which app or window opens links to the sites you choose.',
    sections: [{
      tag: 'compass-links-page',
      path: 'links',
      keywords: 'air traffic control route domain other apps little window',
    }],
    groups: [],
  },
  {
    id: 'shortcuts',
    label: 'Shortcuts',
    color: 'slate',
    blurb: 'Keyboard shortcuts for Compass’s commands.',
    sections: [{
      tag: 'compass-shortcuts-page',
      path: 'shortcuts',
      keywords: 'keyboard keys hotkeys ctrl tab command',
    }],
    groups: [],
  },
  {
    id: 'boosts',
    label: 'Boosts',
    color: 'pink',
    blurb: 'Your own CSS and JavaScript, added to the sites you pick.',
    sections: [{
      tag: 'compass-boosts-page',
      path: 'boosts',
      keywords: 'css javascript custom style site',
    }],
    groups: [],
  },
  {
    id: 'plugins',
    label: 'Plugins',
    color: 'purple',
    blurb: 'Plugins that fill live folders and show what’s new on your tabs.',
    sections: [{
      tag: 'compass-plugins-page',
      path: 'plugins',
      keywords: 'live folders tab previews badges extensions',
    }],
    groups: [],
  },
  {
    id: 'ai',
    label: 'AI',
    color: 'orange',
    blurb: 'Claude in your browser: who answers, and what it may do for you.',
    sections: [{
      tag: 'compass-claude-page',
      path: 'ai',
      keywords: 'claude model api key memory assistant chat',
      searchRows: [
        {id: 'connectionRow', label: 'Connection',
         keywords: 'claude provider anthropic openai gemini chatgpt server ' +
             'gateway endpoint base url test',
         path: 'ai?view=connection'},
        {id: 'apiKey', label: 'API key', keywords: 'claude key token',
         path: 'ai?view=connection'},
        {id: 'backend', label: 'Claude Code',
         keywords: 'connect with subscription', path: 'ai?view=connection'},
        {id: 'model', label: 'Model', keywords: 'claude opus sonnet haiku',
         path: 'ai?view=connection'},
        {id: 'browserScope', label: 'Let Claude use the whole browser',
         keywords: 'tabs windows'},
        {id: 'devtools', label: 'Let Claude use developer tools',
         keywords: 'network storage'},
        {id: 'boosts', label: 'Let Claude make Boosts',
         keywords: 'css javascript'},
        {id: 'plugins', label: 'Let Claude make plugins',
         keywords: 'live folders'},
        {id: 'sidebarRow', label: 'Claude in the sidebar',
         keywords: 'tidy today name link folders shorter tab titles ' +
             'command bar ask apple intelligence',
         path: 'ai?view=sidebar'},
        {id: 'filesRow', label: 'Files', keywords: 'claude folder save',
         path: 'ai?view=files'},
        {id: 'memoryRow', label: 'Memory', keywords: 'claude remember facts',
         path: 'ai?view=memory'},
        {id: 'mcpEnabled', label: 'Allow MCP clients',
         keywords: 'claude code desktop mcp'},
      ],
    }],
    groups: [],
  },
  {
    id: 'privacy',
    label: 'Privacy',
    color: 'blue',
    blurb: 'Cookies, tracking, ad blocking, security and what sites may use.',
    sections: [{
      tag: 'compass-settings-privacy-pane',
      path: 'privacy',
      keywords: '',
      searchRows: [
        {id: 'clearBrowserData', label: 'Clear browsing data',
         keywords: 'delete history cache cookies'},
        {id: 'cookies', label: 'Third-party cookies', keywords: 'tracking'},
        {id: 'doNotTrack', label: 'Ask sites not to track you',
         keywords: 'do not track dnt'},
        {id: 'cleanLinks', label: 'Remove tracking from copied links',
         keywords: 'clean links utm copy url'},
        {id: 'adblockEnabled', label: 'Block ads and trackers',
         keywords: 'ad blocker adblock content blocking trackers'},
        {id: 'filterLists', label: 'Filter lists',
         keywords: 'ad blocker adblock easylist easyprivacy cookie banners ' +
             'ublock custom list'},
        {id: 'safeBrowsing', label: 'Safe Browsing',
         keywords: 'security dangerous malware phishing'},
        {id: 'httpsOnly', label: 'Always use secure connections',
         keywords: 'https'},
        {id: 'secureDns', label: 'Use secure DNS', keywords: 'doh'},
        {id: 'certificates', label: 'Manage certificates'},
        {id: 'permissions', label: 'Site permissions',
         keywords: 'location camera microphone notifications popups ' +
             'javascript site settings'},
        {id: 'sites', label: 'Sites with their own permissions',
         keywords: 'site settings exceptions'},
        {id: 'safetyCheck', label: 'Safety check',
         keywords: 'safety hub compromised passwords extensions'},
      ],
    }],
    groups: [],
  },
  {
    id: 'passwords',
    label: 'Passwords',
    color: 'green',
    blurb: 'Passwords, cards and addresses that Compass fills in for you.',
    sections: [{
      tag: 'compass-settings-passwords-pane',
      path: 'passwords',
      keywords: '',
      searchRows: [
        {id: 'offerToSave', label: 'Offer to save passwords and passkeys'},
        {id: 'autoSignIn', label: 'Sign in automatically'},
        {id: 'passwordManager', label: 'Password Manager',
         keywords: 'checkup import export passkeys'},
        {id: 'savedPasswords', label: 'Saved passwords',
         keywords: 'login username', path: 'passwords?view=passwords'},
        {id: 'saveCards', label: 'Save and fill payment methods',
         keywords: 'credit card'},
        {id: 'addCard', label: 'Add a card', keywords: 'credit card payment',
         path: 'passwords?view=cards'},
        {id: 'saveAddresses', label: 'Save and fill addresses',
         keywords: 'autofill'},
        {id: 'addAddress', label: 'Add an address',
         keywords: 'autofill phone email', path: 'passwords?view=addresses'},
      ],
    }],
    groups: [],
  },
  {
    id: 'advanced',
    label: 'Advanced',
    color: 'slate',
    blurb: 'Performance, accessibility and tools for developers.',
    sections: [
      {
        tag: 'compass-settings-advanced-pane',
        path: 'advanced',
        keywords: '',
        searchRows: [
          {id: 'memorySaver', label: 'Memory saver', keywords: 'performance'},
          {id: 'energySaver', label: 'Energy saver',
           keywords: 'battery performance'},
          {id: 'liveCaption', label: 'Live caption',
           keywords: 'accessibility captions'},
          {id: 'focusHighlight', label: 'Highlight the focused object',
           keywords: 'accessibility'},
          {id: 'caretBrowsing', label: 'Navigate pages with a text cursor',
           keywords: 'caret browsing accessibility'},
          {id: 'reset', label: 'Reset settings', keywords: 'restore defaults'},
        ],
      },
      {
        tag: 'compass-dev-mode-page',
        path: 'devMode',
        keywords: 'development developer localhost devtools cache',
        subpage: {
          group: 'Developers',
          label: 'Development mode',
          icon: 'code',
          color: 'slate',
        },
      },
    ],
    groups: [],
  },
  {
    id: 'about',
    label: 'About',
    color: 'blue',
    blurb: '',
    sections: [{
      tag: 'compass-settings-about-pane',
      path: 'about',
      keywords: '',
      searchRows: [
        {id: 'updates', label: 'Check for updates',
         keywords: 'version about compass update'},
        {id: 'channel', label: 'Update channel',
         keywords: 'beta canary stable'},
        {id: 'licenses', label: 'Open-source licenses',
         keywords: 'credits third party'},
      ],
    }],
    groups: [],
  },
];

// Upstream's paths that name one of the window's panes.
const ALIASES: Record<string, string> = {
  claude: 'ai',
  help: 'about',
};

// The pane and section a path shows: "boosts?add=x" → Boosts; "sync" →
// Profiles' Sync page; "claude" (chrome://settings/claude) → AI; "help" →
// About.
export function resolvePath(path: string):
    {pane: Pane, section: PaneSection|null}|null {
  let name = path.split(/[?#]/)[0]!.replace(/^\/+|\/+$/g, '');
  name = ALIASES[name] || name;
  for (const pane of PANES) {
    const section = pane.sections.find(s => s.path === name) || null;
    if (pane.id === name || section) {
      // Its page, or scrolled to it, if it isn't the pane's own path.
      return {pane, section: section && section.path !== pane.id ? section : null};
    }
  }
  return null;
}
