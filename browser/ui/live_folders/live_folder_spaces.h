// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDER_SPACES_H_
#define COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDER_SPACES_H_

#include "base/functional/callback_forward.h"

namespace compass {

class SidebarNode;
class SpaceList;

// Where LiveFolderService finds a profile's spaces: SpaceService on the
// desktop (live_folder_service_factory.cc), SpacesAndroid on Android
// (spaces_android.cc). Both keep the same SpaceList model; they differ in
// what owns it and in how they know an entry's tab is open.
class LiveFolderSpaces {
 public:
  virtual ~LiveFolderSpaces() = default;

  // Runs |callback| once the saved spaces are in (now, if they are). Not
  // before the profile is made.
  virtual void RunWhenLoaded(base::OnceClosure callback) = 0;
  // The spaces, or null if they're gone (the profile is going away).
  virtual SpaceList* list() = 0;
  // Whether |node| is open in a tab: a live entry that a refresh drops stays
  // until its tab closes.
  virtual bool HasTab(const SidebarNode& node) = 0;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDER_SPACES_H_
