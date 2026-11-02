// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing, repeat} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassNowPlayingElement} from './now_playing.js';
import type {NowPlaying} from './sidebar.mojom-webui.js';
import {speakerIcon} from './speaker_icon.js';

export function getHtml(this: CompassNowPlayingElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${repeat(this.items, (item: NowPlaying) => item.id, (item: NowPlaying) => html`
  <div class="card" role="group" aria-label="${item.title}"
      data-id="${item.id}" ?titled="${this.items.length > 1}">
    <div class="title-row">
      <span class="title" title="${item.title}"
          @click="${this.onShowTabClick_}">${item.title}</span>
      ${item.canPictureInPicture ? html`
        <button class="pip" title="Picture in picture"
            aria-label="Picture in picture"
            @click="${this.onPictureInPictureClick_}">
          <svg viewBox="0 0 16 16" aria-hidden="true">
            <rect class="outline" x="1.5" y="2.5" width="10" height="8"
                rx="1.5"></rect>
            <rect class="outline inner" x="6" y="6.5" width="8.5" height="7"
                rx="1.5"></rect>
          </svg>
        </button>` : nothing}
      <button class="close" title="Close" aria-label="Close"
          @click="${this.onCloseClick_}">
        <svg viewBox="0 0 16 16" aria-hidden="true">
          <path class="outline" d="M4 4l8 8M12 4l-8 8"></path>
        </svg>
      </button>
    </div>
    <div class="controls">
      <button class="show-tab" title="${item.title}"
          aria-label="Go to ${item.title}" @click="${this.onShowTabClick_}">
        <img alt="" src="${this.getFaviconUrl_(item)}">
      </button>
      <button class="previous" title="Previous" aria-label="Previous"
          ?disabled="${!item.canPrevious}" @click="${this.onPreviousClick_}">
        <svg viewBox="0 0 16 16" aria-hidden="true">
          <rect x="3" y="3.5" width="1.6" height="9" rx="0.8"></rect>
          <path d="M13 4.3v7.4a.6.6 0 0 1-.93.5L6.5 8.5a.6.6 0 0 1 0-1l5.57-3.7a.6.6 0 0 1 .93.5z"></path>
        </svg>
      </button>
      <button class="play-pause" title="${item.playing ? 'Pause' : 'Play'}"
          aria-label="${item.playing ? 'Pause' : 'Play'}"
          ?disabled="${!item.canPlayPause}"
          @click="${this.onPlayPauseClick_}">
        ${item.playing ? html`
          <svg viewBox="0 0 16 16" aria-hidden="true">
            <rect x="4" y="3.5" width="2.4" height="9" rx="0.8"></rect>
            <rect x="9.6" y="3.5" width="2.4" height="9" rx="0.8"></rect>
          </svg>` : html`
          <svg viewBox="0 0 16 16" aria-hidden="true">
            <path d="M5 3.9v8.2a.7.7 0 0 0 1.06.6l6.5-4.1a.7.7 0 0 0 0-1.2l-6.5-4.1A.7.7 0 0 0 5 3.9z"></path>
          </svg>`}
      </button>
      <button class="next" title="Next" aria-label="Next"
          ?disabled="${!item.canNext}" @click="${this.onNextClick_}">
        <svg viewBox="0 0 16 16" aria-hidden="true">
          <rect x="11.4" y="3.5" width="1.6" height="9" rx="0.8"></rect>
          <path d="M3 4.3v7.4a.6.6 0 0 0 .93.5L9.5 8.5a.6.6 0 0 0 0-1L3.93 3.8a.6.6 0 0 0-.93.5z"></path>
        </svg>
      </button>
      <button class="audio" ?muted="${item.muted}"
          title="${item.muted ? 'Unmute' : 'Mute'}"
          aria-label="${item.muted ? 'Unmute' : 'Mute'}"
          @click="${this.onMuteClick_}">
        ${speakerIcon(item.muted)}
      </button>
    </div>
  </div>`)}
<!--_html_template_end_-->`;
  // clang-format on
}
