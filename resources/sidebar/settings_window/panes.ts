// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The settings window's panes, in the toolbar's order. A pane shows its
// sections (the window's own elements, and Compass's settings/*_page.ts) and
// groups of rows (docs/design/settings-window.md).

import {i18n} from '../i18n.js';

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
export type TileColor = 'blue'|'indigo'|'purple'|'pink'|'red'|'orange'|'yellow'|
    'green'|'teal'|'gray'|'slate';

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
    label: i18n('generalPaneTitle'),
    color: 'gray',
    blurb: i18n('settingsWindowPaneGeneralBlurb'),
    sections: [{
      tag: 'compass-settings-general-pane',
      path: 'general',
      keywords: '',
      searchRows: [
        {id: 'defaultBrowser', label: i18n('generalPaneDefaultBrowser')},
        {
          id: 'search',
          label: i18n('generalPaneSearchEngine'),
          keywords: 'google bing duckduckgo omnibox',
        },
        {id: 'searchEngines', label: i18n('generalPaneManageSearchEngines')},
        {
          id: 'downloads',
          label: i18n('settingsWindowSearchDownloadLocation'),
          keywords: 'folder'
        },
        {id: 'promptForDownload', label: i18n('generalPanePromptForDownload')},
        {
          id: 'fontSize',
          label: i18n('generalPaneFontSize'),
          keywords: 'text larger smaller'
        },
        {id: 'zoom', label: i18n('generalPanePageZoom')},
        {
          id: 'webkit.webprefs.fonts.standard.Zyyy',
          label: i18n('generalPaneFonts'),
          keywords: 'serif sans-serif fixed typeface',
          path: 'general?view=fonts'
        },
        {
          id: 'minimumFontSize',
          label: i18n('generalPaneMinimumFontSize'),
          path: 'general?view=fonts'
        },
        {id: 'languages', label: i18n('generalPanePreferredLanguages')},
        {id: 'translate', label: i18n('generalPaneOfferTranslate')},
        {
          id: 'spellcheck',
          label: i18n('generalPaneSpellcheck'),
          keywords: 'spellcheck'
        },
        {
          id: 'hardwareAcceleration',
          label: i18n('generalPaneHardwareAcceleration'),
          keywords: 'hardware gpu'
        },
        {id: 'proxy', label: i18n('generalPaneProxy'), keywords: 'network'},
        {
          id: 'pipOnLeavingWindow',
          label: i18n('settingsWindowSearchPipOnLeavingWindow'),
          keywords: 'video call pip'
        },
      ],
    }],
    groups: [],
  },
  {
    id: 'profiles',
    label: i18n('settingsWindowPaneProfiles'),
    color: 'purple',
    blurb: i18n('settingsWindowPaneProfilesBlurb'),
    sections: [
      {
        tag: 'compass-settings-profile-section',
        path: 'profiles',
        keywords: '',
        searchRows: [
          {
            id: 'profileName',
            label: i18n('settingsWindowSearchProfileName'),
            keywords: 'people'
          },
          {
            id: 'addProfile',
            label: i18n('profileSectionNewProfile'),
            keywords: 'add create people',
          },
        ],
      },
      {
        tag: 'compass-sync-page',
        path: 'sync',
        keywords: 'sync devices account sign in pair',
        subpage: {
          group: i18n('settingsWindowSyncAndImport'),
          label: i18n('settingsWindowSync'),
          icon: 'sync',
          color: 'blue',
        },
      },
      {
        tag: 'compass-arc-import-page',
        path: 'importArc',
        keywords: 'import arc chrome safari firefox bookmarks history',
        subpage: {
          group: i18n('settingsWindowSyncAndImport'),
          label: i18n('settingsWindowImport'),
          icon: 'import',
          color: 'green',
        },
      },
    ],
    groups: [],
  },
  {
    id: 'spaces',
    label: i18n('settingsWindowPaneSpaces'),
    color: 'indigo',
    blurb: i18n('settingsWindowPaneSpacesBlurb'),
    sections: [{
      tag: 'compass-settings-spaces-pane',
      path: 'spaces',
      keywords: 'space spaces workspace switcher icon emoji color theme ' +
          'gradient profile archive sync rename reorder delete',
      searchRows: [
        {
          id: 'spaces',
          label: i18n('settingsWindowSearchEditSpaces'),
          keywords: 'rename icon theme'
        },
      ],
    }],
    groups: [],
  },
  {
    id: 'links',
    label: i18n('settingsWindowPaneLinks'),
    color: 'teal',
    blurb: i18n('settingsWindowPaneLinksBlurb'),
    sections: [{
      tag: 'compass-links-page',
      path: 'links',
      keywords: 'air traffic control route domain other apps little window',
    }],
    groups: [],
  },
  {
    id: 'shortcuts',
    label: i18n('settingsWindowPaneShortcuts'),
    color: 'slate',
    blurb: i18n('settingsWindowPaneShortcutsBlurb'),
    sections: [{
      tag: 'compass-shortcuts-page',
      path: 'shortcuts',
      keywords: 'keyboard keys hotkeys ctrl tab command',
    }],
    groups: [],
  },
  {
    id: 'boosts',
    label: i18n('settingsWindowPaneBoosts'),
    color: 'pink',
    blurb: i18n('settingsWindowPaneBoostsBlurb'),
    sections: [{
      tag: 'compass-boosts-page',
      path: 'boosts',
      keywords: 'css javascript custom style site',
    }],
    groups: [],
  },
  {
    id: 'plugins',
    label: i18n('settingsWindowPanePlugins'),
    color: 'purple',
    blurb: i18n('settingsWindowPanePluginsBlurb'),
    sections: [{
      tag: 'compass-plugins-page',
      path: 'plugins',
      keywords: 'live folders tab previews badges extensions',
    }],
    groups: [],
  },
  {
    id: 'ai',
    label: i18n('settingsWindowPaneAi'),
    color: 'orange',
    blurb: i18n('settingsWindowPaneAiBlurb'),
    sections: [{
      tag: 'compass-claude-page',
      path: 'ai',
      keywords: 'claude model api key memory assistant chat',
      searchRows: [
        {
          id: 'connectionRow',
          label: i18n('settingsWindowSearchConnection'),
          keywords: 'claude provider anthropic openai gemini chatgpt server ' +
              'gateway endpoint base url test',
          path: 'ai?view=connection'
        },
        {
          id: 'apiKey',
          label: i18n('settingsWindowSearchApiKey'),
          keywords: 'claude key token',
          path: 'ai?view=connection'
        },
        {
          id: 'backend',
          label: 'Claude Code',
          keywords: 'connect with subscription',
          path: 'ai?view=connection'
        },
        {
          id: 'model',
          label: i18n('settingsWindowSearchModel'),
          keywords: 'claude opus sonnet haiku',
          path: 'ai?view=connection'
        },
        {
          id: 'browserScope',
          label: i18n('settingsWindowSearchBrowserScope'),
          keywords: 'tabs windows'
        },
        {
          id: 'devtools',
          label: i18n('settingsWindowSearchDevtools'),
          keywords: 'network storage'
        },
        {
          id: 'boosts',
          label: i18n('settingsWindowSearchClaudeBoosts'),
          keywords: 'css javascript'
        },
        {
          id: 'plugins',
          label: i18n('settingsWindowSearchClaudePlugins'),
          keywords: 'live folders'
        },
        {
          id: 'sidebarRow',
          label: i18n('settingsWindowSearchClaudeSidebar'),
          keywords: 'tidy today name link folders shorter tab titles ' +
              'command bar ask apple intelligence',
          path: 'ai?view=sidebar'
        },
        {
          id: 'filesRow',
          label: i18n('settingsWindowSearchFiles'),
          keywords: 'claude folder save',
          path: 'ai?view=files'
        },
        {
          id: 'memoryRow',
          label: i18n('settingsWindowSearchMemory'),
          keywords: 'claude remember facts',
          path: 'ai?view=memory'
        },
        {
          id: 'mcpEnabled',
          label: i18n('settingsWindowSearchMcp'),
          keywords: 'claude code desktop mcp'
        },
      ],
    }],
    groups: [],
  },
  {
    id: 'privacy',
    label: i18n('settingsWindowPanePrivacy'),
    color: 'blue',
    blurb: i18n('settingsWindowPanePrivacyBlurb'),
    sections: [{
      tag: 'compass-settings-privacy-pane',
      path: 'privacy',
      keywords: '',
      searchRows: [
        {
          id: 'clearBrowserData',
          label: i18n('privacyPaneClearBrowsingData'),
          keywords: 'delete history cache cookies'
        },
        {
          id: 'cookies',
          label: i18n('privacyPaneThirdPartyCookies'),
          keywords: 'tracking'
        },
        {
          id: 'doNotTrack',
          label: i18n('privacyPaneDoNotTrack'),
          keywords: 'do not track dnt'
        },
        {
          id: 'cleanLinks',
          label: i18n('privacyPaneCleanLinks'),
          keywords: 'clean links utm copy url'
        },
        {
          id: 'adblockEnabled',
          label: i18n('privacyPaneBlockAds'),
          keywords: 'ad blocker adblock content blocking trackers'
        },
        {
          id: 'filterLists',
          label: i18n('privacyPaneFilterLists'),
          keywords: 'ad blocker adblock easylist easyprivacy cookie banners ' +
              'ublock custom list'
        },
        {
          id: 'safeBrowsing',
          label: i18n('privacyPaneSafeBrowsing'),
          keywords: 'security dangerous malware phishing'
        },
        {
          id: 'httpsOnly',
          label: i18n('privacyPaneHttpsOnly'),
          keywords: 'https'
        },
        {id: 'secureDns', label: i18n('privacyPaneSecureDns'), keywords: 'doh'},
        {id: 'certificates', label: i18n('privacyPaneManageCertificates')},
        {
          id: 'permissions',
          label: i18n('privacyPaneSitePermissions'),
          keywords: 'location camera microphone notifications popups ' +
              'javascript site settings'
        },
        {
          id: 'sites',
          label: i18n('privacyPaneSitesWithPermissions'),
          keywords: 'site settings exceptions'
        },
        {
          id: 'safetyCheck',
          label: i18n('privacyPaneSafetyCheck'),
          keywords: 'safety hub compromised passwords extensions'
        },
      ],
    }],
    groups: [],
  },
  {
    id: 'passwords',
    label: i18n('settingsWindowPanePasswords'),
    color: 'green',
    blurb: i18n('settingsWindowPanePasswordsBlurb'),
    sections: [{
      tag: 'compass-settings-passwords-pane',
      path: 'passwords',
      keywords: '',
      searchRows: [
        {id: 'offerToSave', label: i18n('passwordsPaneOfferToSave')},
        {id: 'autoSignIn', label: i18n('passwordsPaneAutoSignIn')},
        {
          id: 'passwordManager',
          label: i18n('passwordsPanePasswordManager'),
          keywords: 'checkup import export passkeys'
        },
        {
          id: 'savedPasswords',
          label: i18n('passwordsPaneSavedPasswords'),
          keywords: 'login username',
          path: 'passwords?view=passwords'
        },
        {
          id: 'saveCards',
          label: i18n('passwordsPaneSaveCards'),
          keywords: 'credit card'
        },
        {
          id: 'addCard',
          label: i18n('passwordsPaneAddCard'),
          keywords: 'credit card payment',
          path: 'passwords?view=cards'
        },
        {
          id: 'saveAddresses',
          label: i18n('passwordsPaneSaveAddresses'),
          keywords: 'autofill'
        },
        {
          id: 'addAddress',
          label: i18n('passwordsPaneAddAddress'),
          keywords: 'autofill phone email',
          path: 'passwords?view=addresses'
        },
      ],
    }],
    groups: [],
  },
  {
    id: 'advanced',
    label: i18n('settingsWindowPaneAdvanced'),
    color: 'slate',
    blurb: i18n('settingsWindowPaneAdvancedBlurb'),
    sections: [
      {
        tag: 'compass-settings-advanced-pane',
        path: 'advanced',
        keywords: '',
        searchRows: [
          {
            id: 'memorySaver',
            label: i18n('advancedPaneMemorySaver'),
            keywords: 'performance'
          },
          {
            id: 'energySaver',
            label: i18n('advancedPaneEnergySaver'),
            keywords: 'battery performance'
          },
          {
            id: 'liveCaption',
            label: i18n('advancedPaneLiveCaption'),
            keywords: 'accessibility captions'
          },
          {
            id: 'focusHighlight',
            label: i18n('advancedPaneFocusHighlight'),
            keywords: 'accessibility'
          },
          {
            id: 'caretBrowsing',
            label: i18n('advancedPaneCaretBrowsing'),
            keywords: 'caret browsing accessibility'
          },
          {
            id: 'reset',
            label: i18n('advancedPaneResetSettings'),
            keywords: 'restore defaults'
          },
        ],
      },
      {
        tag: 'compass-dev-mode-page',
        path: 'devMode',
        keywords: 'development developer localhost devtools cache',
        subpage: {
          group: i18n('settingsWindowDevelopers'),
          label: i18n('settingsWindowDevelopmentMode'),
          icon: 'code',
          color: 'slate',
        },
      },
    ],
    groups: [],
  },
  {
    id: 'about',
    label: i18n('settingsWindowPaneAbout'),
    color: 'blue',
    blurb: '',
    sections: [{
      tag: 'compass-settings-about-pane',
      path: 'about',
      keywords: '',
      searchRows: [
        {
          id: 'updates',
          label: i18n('settingsWindowSearchCheckForUpdates'),
          keywords: 'version about compass update'
        },
        {
          id: 'channel',
          label: i18n('aboutPaneUpdateChannel'),
          keywords: 'beta canary stable'
        },
        {
          id: 'licenses',
          label: i18n('aboutPaneLicenses'),
          keywords: 'credits third party'
        },
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
      return {
        pane,
        section: section && section.path !== pane.id ? section : null
      };
    }
  }
  return null;
}
