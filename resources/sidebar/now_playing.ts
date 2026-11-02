// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {SidebarBrowserProxy} from './browser_proxy.js';
import {getFaviconUrl} from './favicon.js';
import {getCss} from './now_playing.css.js';
import {getHtml} from './now_playing.html.js';
import type {NowPlaying} from './sidebar.mojom-webui.js';
import {NowPlayingAction} from './sidebar.mojom-webui.js';

// The "now playing" widgets at the bottom of the sidebar, as in Arc
// (docs/design/sidebar.md, "Now playing"): one card per tab that plays media
// (or paused it) and isn't shown in the window, nor in picture-in-picture.
// A card shows the tab's icon and the media's controls; its title row (with
// picture-in-picture and close) shows when the pointer is on it, or always
// when there are several cards, to tell them apart.
export class CompassNowPlayingElement extends CrLitElement {
  static get is() {
    return 'compass-now-playing';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      items: {type: Array},
    };
  }

  accessor items: NowPlaying[] = [];

  protected getFaviconUrl_(item: NowPlaying): string {
    return getFaviconUrl(item.url, 32, null);
  }

  protected act_(e: Event, action: NowPlayingAction) {
    e.stopPropagation();
    const id = (e.currentTarget as HTMLElement).closest<HTMLElement>(
                                                   '.card')!.dataset['id']!;
    SidebarBrowserProxy.getInstance().handler.actOnNowPlaying(id, action);
  }

  protected onShowTabClick_(e: Event) {
    this.act_(e, NowPlayingAction.kShowTab);
  }

  protected onPlayPauseClick_(e: Event) {
    const item = this.getItem_(e);
    this.act_(e, item?.playing ? NowPlayingAction.kPause :
                                 NowPlayingAction.kPlay);
  }

  protected onPreviousClick_(e: Event) {
    this.act_(e, NowPlayingAction.kPreviousTrack);
  }

  protected onNextClick_(e: Event) {
    this.act_(e, NowPlayingAction.kNextTrack);
  }

  protected onMuteClick_(e: Event) {
    const item = this.getItem_(e);
    this.act_(e, item?.muted ? NowPlayingAction.kUnmute :
                               NowPlayingAction.kMute);
  }

  protected onPictureInPictureClick_(e: Event) {
    this.act_(e, NowPlayingAction.kPictureInPicture);
  }

  protected onCloseClick_(e: Event) {
    this.act_(e, NowPlayingAction.kDismiss);
  }

  private getItem_(e: Event): NowPlaying|undefined {
    const id = (e.currentTarget as HTMLElement).closest<HTMLElement>(
                                                   '.card')!.dataset['id'];
    return this.items.find(item => item.id === id);
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-now-playing': CompassNowPlayingElement;
  }
}

customElements.define(CompassNowPlayingElement.is, CompassNowPlayingElement);
