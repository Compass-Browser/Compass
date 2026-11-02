// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_activity.h"

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"

namespace compass::claude {

namespace {

// How many sessions act in a tab; gone with the tab.
class ActingCount : public content::WebContentsUserData<ActingCount> {
 public:
  int count = 0;

 private:
  friend WebContentsUserData;
  explicit ActingCount(content::WebContents* contents)
      : WebContentsUserData(*contents) {}
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(ActingCount);

base::ObserverList<ClaudeActivity::Observer>& Observers() {
  static base::NoDestructor<base::ObserverList<ClaudeActivity::Observer>>
      observers;
  return *observers;
}

void Notify(content::WebContents* contents) {
  for (ClaudeActivity::Observer& observer : Observers()) {
    observer.OnClaudeActingChanged(contents);
  }
}

}  // namespace

// static
void ClaudeActivity::Begin(content::WebContents* contents) {
  ActingCount::CreateForWebContents(contents);
  if (ActingCount::FromWebContents(contents)->count++ == 0) {
    Notify(contents);
  }
}

// static
void ClaudeActivity::End(content::WebContents* contents) {
  ActingCount* acting = ActingCount::FromWebContents(contents);
  if (!acting || acting->count == 0) {
    return;
  }
  if (--acting->count == 0) {
    Notify(contents);
  }
}

// static
bool ClaudeActivity::IsActing(const content::WebContents* contents) {
  const ActingCount* acting = ActingCount::FromWebContents(contents);
  return acting && acting->count > 0;
}

// static
void ClaudeActivity::AddObserver(Observer* observer) {
  Observers().AddObserver(observer);
}

// static
void ClaudeActivity::RemoveObserver(Observer* observer) {
  Observers().RemoveObserver(observer);
}

}  // namespace compass::claude
