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
  sections: PaneSection[];
  groups: RowGroup[];
}

export const PANES: Pane[] = [
  {
    id: 'general',
    label: 'General',
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
         keywords: 'serif sans-serif fixed typeface'},
        {id: 'minimumFontSize', label: 'Minimum font size'},
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
    sections: [
      {
        tag: 'compass-settings-profile-section',
        path: 'profiles',
        keywords: '',
        searchRows: [
          {id: 'profileName', label: 'Profile name', keywords: 'people'},
        ],
      },
      {
        tag: 'compass-sync-page',
        path: 'sync',
        keywords: 'sync devices account sign in pair',
      },
      {
        tag: 'compass-arc-import-page',
        path: 'importArc',
        keywords: 'import arc chrome safari firefox bookmarks history',
      },
    ],
    groups: [],
  },
  {
    id: 'links',
    label: 'Links',
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
    sections: [{
      tag: 'compass-claude-page',
      path: 'ai',
      keywords: 'claude model api key memory assistant chat',
    }],
    groups: [],
  },
  {
    id: 'privacy',
    label: 'Privacy',
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
         keywords: 'login username'},
        {id: 'saveCards', label: 'Save and fill payment methods',
         keywords: 'credit card'},
        {id: 'saveCards', label: 'Add a card', keywords: 'credit card payment'},
        {id: 'saveAddresses', label: 'Save and fill addresses',
         keywords: 'autofill'},
        {id: 'saveAddresses', label: 'Add an address',
         keywords: 'autofill phone email'},
      ],
    }],
    groups: [],
  },
  {
    id: 'advanced',
    label: 'Advanced',
    sections: [
      {
        tag: 'compass-settings-advanced-pane',
        path: 'advanced',
        keywords: '',
        searchRows: [
          {id: 'updates', label: 'About Compass',
           keywords: 'version update check'},
          {id: 'channel', label: 'Update channel',
           keywords: 'beta canary stable'},
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
      },
    ],
    groups: [],
  },
];

// The pane and section a path shows: "boosts?add=x" → Boosts; "sync" →
// Profiles, at Sync; "claude" (chrome://settings/claude) → AI.
export function resolvePath(path: string):
    {pane: Pane, section: PaneSection|null}|null {
  let name = path.split(/[?#]/)[0]!.replace(/^\/+|\/+$/g, '');
  if (name === 'claude') {
    name = 'ai';
  }
  for (const pane of PANES) {
    const section = pane.sections.find(s => s.path === name) || null;
    if (pane.id === name || section) {
      // Scrolled to the section if it isn't the pane's own path.
      return {pane, section: section && section.path !== pane.id ? section : null};
    }
  }
  return null;
}
