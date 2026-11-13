// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.extras;

/**
 * A home screen widget, as the iOS app's Other Devices widget: tabs open on the sync chain's other
 * devices, the most recently active device's first.
 */
public class DevicesWidgetProvider extends ListWidgetProvider {
    @Override
    protected String key() {
        return "devices";
    }

    @Override
    protected String prefix() {
        return "compass_devices_";
    }

    @Override
    protected String defaultTitle() {
        return "Other Devices";
    }
}
