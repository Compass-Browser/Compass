// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n} from './i18n.js';
import type {CompassUpdateNoticeElement} from './update_notice.js';

export function getHtml(this: CompassUpdateNoticeElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.ready ? html`
<div class="card" role="group" aria-label="${i18n('updateNoticeTitle')}">
  <div class="title-row">
    <span class="badge" aria-hidden="true">
      <svg viewBox="0 0 16 16">
        <path class="outline" d="M8 12V4.5M4.75 7.5L8 4.25l3.25 3.25"></path>
      </svg>
    </span>
    <span class="text">
      <span class="title">${i18n('updateNoticeTitle')}</span>
      <span class="detail">${i18n('updateNoticeDetail')}</span>
    </span>
    <button class="close" title="${i18n('close')}"
        aria-label="${i18n('close')}" @click="${this.onCloseClick_}">
      <svg viewBox="0 0 16 16" aria-hidden="true">
        <path class="outline" d="M4 4l8 8M12 4l-8 8"></path>
      </svg>
    </button>
  </div>
  <button class="relaunch" @click="${this.onRelaunchClick_}">
    ${i18n('updateNoticeRelaunch')}
  </button>
</div>` : nothing}
<!--_html_template_end_-->`;
  // clang-format on
}
