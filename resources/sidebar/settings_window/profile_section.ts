// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import type {ProfileDetails} from '../sidebar.mojom-webui.js';

import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import {getHtml} from './profile_section.html.js';

// The longest name upstream's profile settings allow.
const MAX_NAME_LENGTH = 500;

// Profiles › This profile (docs/design/settings-window.md, phase 2): its
// name, on the window's handler (as upstream's ProfileInfoHandler and
// ManageProfileHandler do it).
export class CompassSettingsProfileSectionElement extends CrLitElement {
  static get is() {
    return 'compass-settings-profile-section';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      info_: {type: Object},
      maxNameLength: {type: Number},
    };
  }

  protected accessor info_: ProfileDetails|null = null;
  protected accessor maxNameLength: number = MAX_NAME_LENGTH;

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;

  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    this.handler_.getProfile().then(
        ({profile}: {profile: ProfileDetails}) => this.info_ = profile);
    SettingsWindowBrowserProxy.getInstance()
        .callbackRouter.profileChanged.addListener(
            (profile: ProfileDetails) => this.info_ = profile);
  }

  protected getName_(): string {
    return this.info_?.name || '';
  }

  // The name is saved when the field is left or Enter is pressed; an empty
  // one goes back to the saved name.
  protected onNameChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    const name = input.value.trim();
    if (!name) {
      input.value = this.getName_();
      return;
    }
    if (name !== this.getName_()) {
      this.handler_.setProfileName(name);
    }
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-settings-profile-section': CompassSettingsProfileSectionElement;
  }
}

customElements.define(
    CompassSettingsProfileSectionElement.is,
    CompassSettingsProfileSectionElement);
