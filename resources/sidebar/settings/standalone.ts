// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The standalone pages (chrome://compass-sync, chrome://compass-settings,
// chrome://compass-import): one of Compass's settings sections alone, as
// the end-to-end steps and browser tests drive them.
import './arc_import_page.js';
import './shortcuts_page.js';
import './sync_page.js';

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';

ColorChangeUpdater.forDocument().start();
