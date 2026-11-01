// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SYNC_COMPASS_SYNC_PAGE_HANDLER_H_
#define COMPASS_BROWSER_UI_WEBUI_SYNC_COMPASS_SYNC_PAGE_HANDLER_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "compass/browser/sync/sync_controller.h"
#include "compass/browser/ui/webui/sync/compass_sync.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace compass {

// chrome://compass-sync's browser side: forwards the page's requests to the
// profile's SyncController, and pushes its changes to the page.
class CompassSyncPageHandler : public sync::mojom::PageHandler,
                               public sync::SyncController::Observer {
 public:
  // Asks the user to prove they're the Mac's user (Touch ID, password) and
  // runs the callback with the answer.
  using Reauthenticator =
      base::RepeatingCallback<void(base::OnceCallback<void(bool)>)>;

  // |controller| must outlive |this|.
  CompassSyncPageHandler(
      mojo::PendingReceiver<sync::mojom::PageHandler> receiver,
      mojo::PendingRemote<sync::mojom::Page> page,
      sync::SyncController* controller,
      Reauthenticator reauthenticator);
  CompassSyncPageHandler(const CompassSyncPageHandler&) = delete;
  CompassSyncPageHandler& operator=(const CompassSyncPageHandler&) = delete;
  ~CompassSyncPageHandler() override;

  // sync::mojom::PageHandler:
  void GetState(GetStateCallback callback) override;
  void StartSync(const std::string& server_url,
                 StartSyncCallback callback) override;
  void JoinWithRecoveryCode(const std::string& server_url,
                            const std::string& words,
                            JoinWithRecoveryCodeCallback callback) override;
  void JoinWithPairingCode(const std::string& server_url,
                           const std::string& code) override;
  void AddDevice() override;
  void AnswerPairing(bool allow) override;
  void CancelPairing() override;
  void Leave() override;
  void SetDataTypeEnabled(sync::mojom::DataType type, bool enabled) override;
  void RevealRecoveryCode(RevealRecoveryCodeCallback callback) override;
  void ResetSync(ResetSyncCallback callback) override;
  void Restart() override;
  void GetProfileMapping(GetProfileMappingCallback callback) override;
  void SetProfileMapping(
      std::vector<sync::mojom::ProfileMappingEntryPtr> mapping,
      SetProfileMappingCallback callback) override;

  // sync::SyncController::Observer:
  void OnSyncControllerChanged() override;

  static sync::mojom::StatusPtr ToMojo(const sync::SyncStatus& status);
  // |server_url| goes into the invite while A shows the code.
  static sync::mojom::PairingPtr ToMojo(const sync::PairingStatus& pairing,
                                        const std::string& server_url);
  // The QR payload a phone scans to join: "compass-pair:?server=" +
  // the escaped |server_url| + "&code=" + |code|. Empty if either is.
  static std::string PairingInviteUrl(const std::string& server_url,
                                      const std::string& code);
  static sync::mojom::DetailsPtr ToMojo(
      const std::vector<sync::SyncDevice>& devices,
      const std::vector<sync::SyncDataTypeState>& types);
  static sync::mojom::ProfileMappingPtr ToMojo(
      const sync::ProfileMappingStatus& mapping);

 private:
  void OnReauthenticated(RevealRecoveryCodeCallback callback,
                         bool authenticated);
  sync::mojom::DetailsPtr GetDetails() const;
  sync::mojom::PairingPtr GetPairing() const;

  mojo::Receiver<sync::mojom::PageHandler> receiver_;
  mojo::Remote<sync::mojom::Page> page_;
  const raw_ptr<sync::SyncController> controller_;
  Reauthenticator reauthenticator_;
  bool reauthenticating_ = false;
  base::ScopedObservation<sync::SyncController, sync::SyncController::Observer>
      observation_{this};
  base::WeakPtrFactory<CompassSyncPageHandler> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SYNC_COMPASS_SYNC_PAGE_HANDLER_H_
