// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {EaselObject} from './easel.mojom-webui.js';

// One object before and after an edit: null before for one added, null
// after for one deleted.
export interface Change {
  before: EaselObject|null;
  after: EaselObject|null;
}

const MAX_STEPS = 200;

// The page's undo and redo (not synced): each step is the changes of one
// edit, a drag of a selection being one.
export class History {
  private undo_: Change[][] = [];
  private redo_: Change[][] = [];

  push(changes: Change[]) {
    if (changes.length === 0) {
      return;
    }
    this.undo_.push(changes);
    if (this.undo_.length > MAX_STEPS) {
      this.undo_.shift();
    }
    this.redo_ = [];
  }

  // The changes that undo the last step, which then can be redone.
  undo(): Change[]|null {
    const step = this.undo_.pop();
    if (!step) {
      return null;
    }
    this.redo_.push(step);
    return invert(step);
  }

  redo(): Change[]|null {
    const step = this.redo_.pop();
    if (!step) {
      return null;
    }
    this.undo_.push(step);
    return step;
  }

  canUndo(): boolean {
    return this.undo_.length > 0;
  }

  canRedo(): boolean {
    return this.redo_.length > 0;
  }
}

function invert(changes: Change[]): Change[] {
  return changes.map(c => ({before: c.after, after: c.before})).reverse();
}
