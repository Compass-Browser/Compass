// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_EASELS_EASEL_STORE_H_
#define COMPASS_BROWSER_EASELS_EASEL_STORE_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/important_file_writer.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"

class Profile;

namespace compass::easels {

// docs/design/easels.md, "What a board is".
inline constexpr size_t kMaxEasels = 200;
inline constexpr size_t kMaxObjectsPerEasel = 1000;
inline constexpr size_t kMaxTextChars = 20000;
inline constexpr size_t kMaxFreehandPoints = 4000;
inline constexpr size_t kMaxTitleChars = 200;
inline constexpr size_t kMaxAssetBytes = 512 * 1024;
inline constexpr int kMaxAssetPixels = 2048;
inline constexpr size_t kMaxAssetBytesTotal = 64 * 1024 * 1024;
// An image no object points at goes after this long.
inline constexpr base::TimeDelta kUnusedAssetLifetime = base::Days(30);

// CompassEaselShape.kind.
enum ShapeKind {
  kRectangle = 1,
  kCircle = 2,
  kArrow = 3,
  kFreehand = 4,
};

// The URL of a board's tab: "compass://easel/<uuid>", and the board of such
// a URL (compass:// or chrome://compass-easel/), or "" for any other URL.
std::string EaselUrl(std::string_view uuid);
std::string EaselUuidFromUrl(std::string_view url);

// CompassEaselAsset.uuid for an image with this SHA-256 (lowercase hex).
std::string AssetUuid(std::string_view sha256);
// SHA-256 of |data| as lowercase hex.
std::string Sha256Hex(std::string_view data);

// Microseconds since the Windows epoch, as the sync messages keep times.
int64_t ToSyncTime(base::Time time);
base::Time FromSyncTime(int64_t micros);

// Checks and normalizes an object a user (or an import) saves: the box, the
// content (exactly one kind, the limits, a freehand stroke simplified to
// the limit). An error says what's wrong.
base::expected<sync_pb::CompassEaselObject, std::string> NormalizeObject(
    sync_pb::CompassEaselObject object);

// A profile's Easels (docs/design/easels.md): boards, their objects and
// their images, as the sync messages. Boards and objects are in
// <profile>/Compass Easels/easels.pb, images in
// <profile>/Compass Easels/assets/<sha256>. Not in Incognito or Guest.
// Syncs with COMPASS_SPACES (docs/design/sync.md, "Easels").
class EaselStore : public KeyedService,
                   public base::ImportantFileWriter::DataSerializer {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // Something of |easel_uuid| changed (the board, an object, or it was
    // deleted); "" when it could be any (loading, sync).
    virtual void OnEaselChanged(const std::string& easel_uuid) {}
    // Any change, local or from sync: what sync listens to.
    virtual void OnEaselsChanged() {}
  };

  // What the sync bridge keeps: every board, object and image, as entities
  // (images without their bytes: FindAsset() has them).
  struct Entities {
    Entities();
    Entities(Entities&&);
    Entities& operator=(Entities&&);
    ~Entities();
    std::vector<sync_pb::CompassEasel> easels;
    std::vector<sync_pb::CompassEaselObject> objects;
    std::vector<sync_pb::CompassEaselAsset> assets;
  };

  explicit EaselStore(const base::FilePath& dir);
  EaselStore(const EaselStore&) = delete;
  EaselStore& operator=(const EaselStore&) = delete;
  ~EaselStore() override;

  // Null for profiles without Easels (Incognito, Guest).
  static EaselStore* Get(Profile* profile);

  // |done| runs once the files are read, or at once if they were.
  void WhenLoaded(base::OnceClosure done);
  bool loaded() const { return loaded_; }

  // Boards, newest first (create time).
  std::vector<const sync_pb::CompassEasel*> Easels() const;
  const sync_pb::CompassEasel* FindEasel(std::string_view uuid) const;
  // A board's objects, lowest z first.
  std::vector<const sync_pb::CompassEaselObject*> ObjectsOf(
      std::string_view easel_uuid) const;
  const sync_pb::CompassEaselObject* FindObject(std::string_view uuid) const;
  // An image's bytes and type, by SHA-256.
  const sync_pb::CompassEaselAsset* FindAsset(std::string_view sha256) const;

  // A new board (|uuid| empty: a random one). Fails when full or loading.
  base::expected<sync_pb::CompassEasel, std::string> CreateEasel(
      std::string title,
      std::string uuid = std::string());
  bool RenameEasel(std::string_view uuid, std::string title);
  bool SetPalette(std::string_view uuid, std::string palette);
  // Deletes the board and its objects. False if there's no such board.
  bool DeleteEasel(std::string_view uuid);

  // Adds |object| (no uuid: a new one) or replaces the one with its uuid,
  // which keeps its create time. Its board must exist. The saved object, or
  // why not.
  base::expected<sync_pb::CompassEaselObject, std::string> SaveObject(
      sync_pb::CompassEaselObject object);
  // Several at once (a drag of a selection), one notification.
  base::expected<std::vector<sync_pb::CompassEaselObject>, std::string>
  SaveObjects(std::vector<sync_pb::CompassEaselObject> objects);
  bool DeleteObject(std::string_view uuid);

  // Keeps an image already prepared by PrepareImage() (easel_images.h): at
  // most kMaxAssetBytes and kMaxAssetPixels a side. Returns its SHA-256.
  // An image already there is kept once.
  base::expected<std::string, std::string> AddAsset(std::string data,
                                                    std::string mime_type);

  // Sync. Entities() is everything (nullopt until loaded). ApplyRemote():
  // |changed| replace or add by uuid unless the local one was updated later
  // (images are kept once, they never change); |deleted| are client tags of
  // any of the three kinds. Before the files are read, waits for them.
  std::optional<Entities> GetEntities() const;
  void ApplyRemote(Entities changed, const std::set<std::string>& deleted);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Writes pending changes now (shutdown, tests).
  void Flush();

  base::WeakPtr<EaselStore> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // ImportantFileWriter::DataSerializer:
  std::optional<std::string> SerializeData() override;

  // KeyedService:
  void Shutdown() override;

  // Reads |dir| as a store does, for tests: what's there, newest first.
  struct ReadResult;

 private:
  void OnRead(std::unique_ptr<ReadResult> result);
  // |easel_uuid| "" for any.
  void Changed(const std::string& easel_uuid);
  void DropUnusedAssets();
  size_t AssetBytes() const;

  base::FilePath dir_;
  scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  base::ImportantFileWriter writer_;
  bool loaded_ = false;
  std::vector<base::OnceClosure> when_loaded_;
  std::map<std::string, sync_pb::CompassEasel> easels_;
  std::map<std::string, sync_pb::CompassEaselObject> objects_;
  // By SHA-256.
  std::map<std::string, sync_pb::CompassEaselAsset> assets_;
  base::ObserverList<Observer> observers_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<EaselStore> weak_factory_{this};
};

class EaselStoreFactory : public ProfileKeyedServiceFactory {
 public:
  static EaselStoreFactory* GetInstance();
  // Null for profiles without Easels.
  static EaselStore* GetForProfile(Profile* profile);

 private:
  friend base::NoDestructor<EaselStoreFactory>;

  EaselStoreFactory();
  ~EaselStoreFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
};

}  // namespace compass::easels

#endif  // COMPASS_BROWSER_EASELS_EASEL_STORE_H_
