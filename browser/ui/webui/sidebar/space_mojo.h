// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SIDEBAR_SPACE_MOJO_H_
#define COMPASS_BROWSER_UI_WEBUI_SIDEBAR_SPACE_MOJO_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "compass/browser/window_sync/space_directory.h"

class Profile;

namespace compass {

// Spaces as the sidebar's space menu and the settings window's Spaces pane
// show and edit them: both list every profile's (SpaceDirectory), and edit
// any of them.

// The longest archive delay offered, a year.
inline constexpr uint32_t kMaxArchiveAfterHours = 365 * 24;

// Null for grey.
sidebar::mojom::SpaceColorPtr ToMojoColor(SpaceColor color);
// Null for none.
sidebar::mojom::SpaceIconPtr ToMojoIcon(const SpaceIcon& icon);
// |entry|'s space, as a page shows it; |own_profile| is the page's.
sidebar::mojom::SpacePtr ToMojoSpace(const SpaceDirectory::Entry& entry,
                                     const Profile* own_profile);
// The profiles a space can be assigned to, as the profile menu lists them.
std::vector<sidebar::mojom::ProfileOptionPtr> GetProfileOptions();

// The path of the profile a page names by its directory's name
// (ProfileOption::id), or nullopt for anything but a plain directory name.
std::optional<base::FilePath> ProfilePathFromId(const std::string& id);

// Whether |profile|'s |space| is a viewer's shared space, which can't be
// changed here.
bool IsViewOnlySpace(Profile* profile, const SpaceModel& space);

// The edits pages make. Each checks its value, and ignores a bad one.
void RenameSpace(SpaceModel& space, const std::string& name);
void SetSpaceColor(SpaceModel& space,
                   const sidebar::mojom::SpaceColorPtr& color);
void SetSpaceGradient(SpaceModel& space,
                      const std::vector<sidebar::mojom::SpaceColorPtr>& colors);
void SetSpaceIntensity(SpaceModel& space, double intensity);
void SetSpaceGrain(SpaceModel& space, double grain);
void SetSpaceTranslucency(SpaceModel& space, double translucency);
void SetSpaceAppearance(SpaceModel& space,
                        sidebar::mojom::SpaceAppearance appearance);
void SetSpaceIcon(SpaceModel& space, const sidebar::mojom::SpaceIconPtr& icon);
void SetSpaceArchiveAfter(SpaceModel& space, uint32_t hours);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SIDEBAR_SPACE_MOJO_H_
