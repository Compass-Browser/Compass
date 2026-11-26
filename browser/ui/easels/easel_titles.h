// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_EASELS_EASEL_TITLES_H_
#define COMPASS_BROWSER_UI_EASELS_EASEL_TITLES_H_

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "compass/browser/easels/easel_store.h"

namespace compass {

class SpaceModel;

namespace easels {

// What a board without a title is called (a new one, until it's named), in
// the browser's language.
std::string NewEaselTitle();

// A board's title as its tab shows it: its own, or NewEaselTitle().
std::u16string EaselTabTitle(const sync_pb::CompassEasel& easel);

// Gives |space|'s tab entries of the board |easel_uuid| (by their URL or
// pinned URL, compass://easel/<uuid>) the title |title|, and their pinned
// URL's title too, so an entry without a tab shows the board's name. A name
// the user gave the entry (custom_title) stays in front. Returns how many
// entries changed.
size_t ApplyEaselTitle(SpaceModel& space,
                       std::string_view easel_uuid,
                       const std::u16string& title);

// docs/design/easels.md, "In the sidebar": a board's tab has the board's
// title. A live tab gets it from the editor's page; this keeps the saved
// title of every entry in step when a board is renamed, here or on another
// device, for entries without a tab. One per profile, over its spaces and
// favorites: the desktop's SpaceService and Android's SpacesAndroid.
class EaselTitleFollower : public EaselStore::Observer {
 public:
  // The spaces and the favorites to keep in step, asked each time.
  using ModelsGetter = base::RepeatingCallback<std::vector<SpaceModel*>()>;

  EaselTitleFollower(EaselStore* store, ModelsGetter models);
  EaselTitleFollower(const EaselTitleFollower&) = delete;
  EaselTitleFollower& operator=(const EaselTitleFollower&) = delete;
  ~EaselTitleFollower() override;

  // EaselStore::Observer:
  void OnEaselChanged(const std::string& easel_uuid) override;

 private:
  // Applies |easel|'s title if it changed since it was last seen.
  void Follow(const sync_pb::CompassEasel& easel);

  // A keyed service, gone before the profile's user data (where the
  // desktop's SpaceService keeps this) is.
  base::WeakPtr<EaselStore> store_;
  ModelsGetter models_;
  // By board: the title last applied.
  std::map<std::string, std::u16string> titles_;
};

}  // namespace easels
}  // namespace compass

#endif  // COMPASS_BROWSER_UI_EASELS_EASEL_TITLES_H_
