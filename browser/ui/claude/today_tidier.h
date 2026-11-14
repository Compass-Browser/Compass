// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_TODAY_TIDIER_H_
#define COMPASS_BROWSER_UI_CLAUDE_TODAY_TIDIER_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "compass/browser/claude/claude_quick.h"
#include "compass/browser/sidebar/space_model.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace compass {

// Tidy Today (docs/design/ai-sidebar.md, "Tidy Today"): on the user's
// request ("Tidy" in Today's header or the command bar), one request to
// Claude with a space's loose Today entries (their titles and hosts), whose
// reply groups them into up to 6 named folders, applied as one change
// (SpaceModel::TidyToday()) that Undo takes back. Only with "Tidy Today" on
// (prefs::kTidyToday, and Claude set up: ClaudeQuick); never off the
// record. Never closes, archives or moves tabs to other spaces.
class TodayTidier : public KeyedService {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // A tidy of space |space_id| started or ended (IsBusy()).
    virtual void OnTidyBusyChanged(const base::Uuid& space_id) {}
    // Space |space_id| was tidied into |folders| folders; Undo(|undo_id|)
    // takes it back.
    virtual void OnTidied(const base::Uuid& space_id,
                          int undo_id,
                          size_t folders) {}
    // Tidying space |space_id| didn't work, or found nothing to group:
    // |message| says so.
    virtual void OnTidyFailed(const base::Uuid& space_id,
                              const std::string& message) {}
  };

  // Fewer loose entries than this aren't worth tidying.
  static constexpr size_t kMinEntries = 5;
  // The most entries a request lists (Today's topmost).
  static constexpr size_t kMaxEntries = 40;
  // The most tidies Undo can still take back.
  static constexpr size_t kMaxUndos = 10;

  TodayTidier(Profile* profile, claude::ClaudeQuick* quick);
  TodayTidier(const TodayTidier&) = delete;
  TodayTidier& operator=(const TodayTidier&) = delete;
  ~TodayTidier() override;

  // Null off the record and for guests.
  static TodayTidier* Get(Profile* profile);

  // |space|'s loose Today entries: the tabs and splits at the top of Today
  // (not folders or what's in them), in order.
  static std::vector<const SidebarNode*> LooseEntries(const SpaceModel& space);
  // The user message for |entries|: a line each, "<n>. <title> (<host>)",
  // with n from 1; a split's titles joined by " + ". Titles and hosts only.
  static std::string DescribeEntries(
      const std::vector<const SidebarNode*>& entries);

  // Whether "Tidy" is offered at all ("Tidy Today" on, and Claude set up).
  bool IsEnabled() const;
  // Whether |space| can be tidied now: IsEnabled(), a space (not
  // Favorites) with kMinEntries loose entries or more, not IsBusy().
  bool CanTidy(const SpaceModel& space) const;
  // Whether a tidy of |space_id| waits for Claude.
  bool IsBusy(const base::Uuid& space_id) const;

  // Asks Claude to tidy |space|'s Today, if CanTidy(). Observers hear how it
  // went. A user action: no hourly limit (ClaudeQuick, background false).
  void Tidy(SpaceModel& space);
  // Takes tidy |undo_id| back (SpaceModel::UndoTidy()). False if it's gone,
  // or nothing is left to undo.
  bool Undo(int undo_id);

  // Applies Claude's |reply| to tidy |space| as Tidy() does, |ids| being
  // the entries the request listed, in order. Public for tests.
  void Apply(SpaceModel& space,
             const std::vector<base::Uuid>& ids,
             const base::DictValue& reply);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // KeyedService:
  void Shutdown() override;

 private:
  struct Undoable {
    base::Uuid space_id;
    SpaceModel::TodayTidy tidy;
  };

  SpaceModel* FindSpace(const base::Uuid& space_id);
  void OnReply(const base::Uuid& space_id,
               std::vector<base::Uuid> ids,
               claude::ClaudeQuick::Result result);
  void SetBusy(const base::Uuid& space_id, bool busy);
  void Fail(const base::Uuid& space_id, const std::string& message);

  const raw_ptr<Profile> profile_;
  const raw_ptr<claude::ClaudeQuick> quick_;
  std::set<base::Uuid> busy_;
  // Tidies Undo can take back, oldest first.
  std::map<int, Undoable> undos_;
  int next_undo_id_ = 1;
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<TodayTidier> weak_factory_{this};
};

class TodayTidierFactory : public ProfileKeyedServiceFactory {
 public:
  static TodayTidierFactory* GetInstance();
  static TodayTidier* GetForProfile(Profile* profile);

 private:
  friend base::NoDestructor<TodayTidierFactory>;

  TodayTidierFactory();
  ~TodayTidierFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_CLAUDE_TODAY_TIDIER_H_
