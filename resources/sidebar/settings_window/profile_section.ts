// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n, i18nF, i18nPlural} from '../i18n.js';
import type {ProfileDetails} from '../sidebar.mojom-webui.js';

import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import {getHtml} from './profile_section.html.js';

// The longest name upstream's profile settings allow.
const MAX_NAME_LENGTH = 500;

// Profiles (docs/design/settings-window.md): every profile, whichever one
// the window is, to rename, add or delete. The window's own goes first.
export class CompassSettingsProfileSectionElement extends CrLitElement {
  static get is() {
    return 'compass-settings-profile-section';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      profiles_: {type: Array},
      confirmingDeleteId_: {type: String},
      maxNameLength: {type: Number},
    };
  }

  protected accessor profiles_: ProfileDetails[] = [];
  // The profile whose deletion is asked to be confirmed.
  protected accessor confirmingDeleteId_: string|null = null;
  protected accessor maxNameLength: number = MAX_NAME_LENGTH;

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;
  private listenerId_: number|null = null;

  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    this.handler_.getProfiles().then(
        ({profiles}: {profiles: ProfileDetails[]}) =>
            this.setProfiles_(profiles));
    this.listenerId_ =
        SettingsWindowBrowserProxy.getInstance()
            .callbackRouter.profilesChanged.addListener(
                (profiles: ProfileDetails[]) => this.setProfiles_(profiles));
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    if (this.listenerId_ !== null) {
      SettingsWindowBrowserProxy.getInstance().callbackRouter.removeListener(
          this.listenerId_);
      this.listenerId_ = null;
    }
  }

  private setProfiles_(profiles: ProfileDetails[]) {
    // The window's own first, then the others as the profile menu lists them.
    this.profiles_ = [
      ...profiles.filter(profile => profile.current),
      ...profiles.filter(profile => !profile.current),
    ];
    if (!profiles.some(profile => profile.id === this.confirmingDeleteId_)) {
      this.confirmingDeleteId_ = null;
    }
  }

  protected getSpaceCountText_(profile: ProfileDetails): string {
    const spaces = i18nPlural('profileSectionSpaceCount', profile.spaceCount);
    return profile.current ? i18nF('profileSectionCurrentProfile', spaces) :
                             spaces;
  }

  protected canDelete_(): boolean {
    return this.profiles_.length > 1;
  }

  protected isConfirmingDelete_(profile: ProfileDetails): boolean {
    return this.confirmingDeleteId_ === profile.id;
  }

  protected getDeleteText_(profile: ProfileDetails): string {
    const text = i18nPlural(
        'profileSectionDeleteConfirm', profile.spaceCount, profile.name);
    return profile.current ?
        `${text} ${i18n('profileSectionDeleteClosesWindow')}` :
        text;
  }

  private getProfileId_(e: Event): string {
    return (e.currentTarget as HTMLElement).dataset['profileId']!;
  }

  // The name is saved when the field is left or Enter is pressed; an empty
  // one goes back to the saved name.
  protected onNameChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    const id = input.dataset['profileId']!;
    const saved = this.profiles_.find(profile => profile.id === id)?.name ?? '';
    const name = input.value.trim();
    if (!name) {
      input.value = saved;
      return;
    }
    if (name !== saved) {
      this.handler_.setProfileName(id, name);
    }
  }

  protected onNameKeydown_(e: KeyboardEvent) {
    if (e.key === 'Enter') {
      (e.target as HTMLInputElement).blur();
    }
  }

  protected onAddClick_() {
    this.handler_.createProfile();
  }

  protected onDeleteClick_(e: Event) {
    this.confirmingDeleteId_ = this.getProfileId_(e);
  }

  protected onDeleteCancelClick_() {
    this.confirmingDeleteId_ = null;
  }

  protected onDeleteConfirmClick_(e: Event) {
    this.handler_.deleteProfile(this.getProfileId_(e));
    this.confirmingDeleteId_ = null;
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
