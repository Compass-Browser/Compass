// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The settings window's Passwords pane (phase 4,
// docs/design/settings-window.md): saved passwords and passkeys through
// upstream's PasswordsPrivateDelegate (the password manager page's, which
// asks the system to authenticate before giving a password and clears copied
// ones), payment cards and addresses through the PersonalDataManager.

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/autofill/personal_data_manager_factory.h"
#include "chrome/browser/extensions/api/passwords_private/passwords_private_delegate_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/ui/webui/settings/settings_window_handler.h"
#include "components/autofill/core/browser/data_manager/addresses/address_data_manager.h"
#include "components/autofill/core/browser/data_manager/payments/payments_data_manager.h"
#include "components/autofill/core/browser/data_model/addresses/autofill_profile.h"
#include "components/autofill/core/browser/data_model/payments/credit_card.h"
#include "components/autofill/core/browser/field_types.h"

namespace compass {

namespace passwords_private = extensions::api::passwords_private;

extensions::PasswordsPrivateDelegate*
SettingsWindowHandler::GetPasswordsDelegate() {
  if (!passwords_delegate_) {
    passwords_delegate_ =
        extensions::PasswordsPrivateDelegateFactory::GetForBrowserContext(
            profile_, /*create=*/true);
    if (passwords_delegate_) {
      saved_passwords_observation_.Observe(
          passwords_delegate_->GetSavedPasswordsPresenter());
    }
  }
  return passwords_delegate_.get();
}

autofill::PersonalDataManager* SettingsWindowHandler::GetPersonalDataManager() {
  return autofill::PersonalDataManagerFactory::GetForBrowserContext(profile_);
}

// Passwords
// --------------------------------------------------------------------

void SettingsWindowHandler::GetPasswords(GetPasswordsCallback callback) {
  extensions::PasswordsPrivateDelegate* delegate = GetPasswordsDelegate();
  if (!delegate) {
    std::move(callback).Run({});
    return;
  }
  delegate->GetSavedPasswordsList(base::BindOnce(
      [](base::WeakPtr<SettingsWindowHandler> handler,
         GetPasswordsCallback callback,
         const extensions::PasswordsPrivateDelegate::UiEntries& entries) {
        std::vector<sidebar::mojom::SavedPasswordPtr> passwords;
        if (handler) {
          handler->password_stores_.clear();
        }
        for (const passwords_private::PasswordUiEntry& entry : entries) {
          if (handler) {
            handler->password_stores_[entry.id] = entry.stored_in;
          }
          std::string site;
          if (!entry.affiliated_domains.empty()) {
            site = entry.affiliated_domains.front().name;
          }
          passwords.push_back(sidebar::mojom::SavedPassword::New(
              entry.id, site,
              entry.username.empty() && entry.display_name ? *entry.display_name
                                                           : entry.username,
              entry.is_passkey));
        }
        std::move(callback).Run(std::move(passwords));
      },
      weak_factory_.GetWeakPtr(), std::move(callback)));
}

void SettingsWindowHandler::RevealPassword(int32_t id,
                                           RevealPasswordCallback callback) {
  extensions::PasswordsPrivateDelegate* delegate = GetPasswordsDelegate();
  if (!delegate) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  delegate->RequestPlaintextPassword(
      id, passwords_private::PlaintextReason::kView,
      base::BindOnce(
          [](RevealPasswordCallback callback,
             std::optional<std::u16string> password) {
            std::move(callback).Run(password ? std::optional<std::string>(
                                                   base::UTF16ToUTF8(*password))
                                             : std::nullopt);
          },
          std::move(callback)));
}

void SettingsWindowHandler::CopyPassword(int32_t id,
                                         CopyPasswordCallback callback) {
  extensions::PasswordsPrivateDelegate* delegate = GetPasswordsDelegate();
  if (!delegate) {
    std::move(callback).Run(false);
    return;
  }
  // The delegate copies it (and clears it later); the page never gets it.
  delegate->RequestPlaintextPassword(
      id, passwords_private::PlaintextReason::kCopy,
      base::BindOnce(
          [](CopyPasswordCallback callback,
             std::optional<std::u16string> result) {
            std::move(callback).Run(result.has_value());
          },
          std::move(callback)));
}

void SettingsWindowHandler::RemovePassword(int32_t id) {
  extensions::PasswordsPrivateDelegate* delegate = GetPasswordsDelegate();
  auto store = password_stores_.find(id);
  if (!delegate || store == password_stores_.end()) {
    return;
  }
  delegate->RemoveCredential(id, store->second);
}

void SettingsWindowHandler::OnSavedPasswordsChanged(
    const password_manager::PasswordStoreChangeList& changes) {
  page_->PasswordsChanged();
}

// Payment cards and addresses
// ------------------------------------------------------

void SettingsWindowHandler::GetCards(GetCardsCallback callback) {
  std::vector<sidebar::mojom::SavedCardPtr> cards;
  if (autofill::PersonalDataManager* data = GetPersonalDataManager()) {
    for (const autofill::CreditCard* card :
         data->payments_data_manager().GetCreditCards()) {
      cards.push_back(sidebar::mojom::SavedCard::New(
          card->guid(),
          base::UTF16ToUTF8(card->nickname().empty()
                                ? card->CardNameForAutofillDisplay()
                                : card->nickname()),
          base::UTF16ToUTF8(card->LastFourDigits()),
          base::UTF16ToUTF8(card->AbbreviatedExpirationDateForDisplay(
              /*with_prefix=*/false)),
          autofill::CreditCard::IsLocalCard(card)));
    }
  }
  std::move(callback).Run(std::move(cards));
}

void SettingsWindowHandler::RemoveCard(const std::string& guid) {
  autofill::PersonalDataManager* data = GetPersonalDataManager();
  if (!data) {
    return;
  }
  // Only Compass's own: account cards are managed in the account.
  for (const autofill::CreditCard* card :
       data->payments_data_manager().GetCreditCards()) {
    if (card->guid() == guid && autofill::CreditCard::IsLocalCard(card)) {
      data->payments_data_manager().RemoveByGUID(guid);
      return;
    }
  }
}

void SettingsWindowHandler::GetAddresses(GetAddressesCallback callback) {
  std::vector<sidebar::mojom::SavedAddressPtr> addresses;
  if (autofill::PersonalDataManager* data = GetPersonalDataManager()) {
    for (const autofill::AutofillProfile* profile :
         data->address_data_manager().GetProfiles()) {
      std::u16string summary =
          profile->GetRawInfo(autofill::ADDRESS_HOME_LINE1);
      const std::u16string city =
          profile->GetRawInfo(autofill::ADDRESS_HOME_CITY);
      if (!city.empty()) {
        summary = summary.empty() ? city : base::StrCat({summary, u", ", city});
      }
      std::u16string name = profile->GetRawInfo(autofill::NAME_FULL);
      if (name.empty()) {
        name = profile->GetRawInfo(autofill::EMAIL_ADDRESS);
      }
      addresses.push_back(sidebar::mojom::SavedAddress::New(
          profile->guid(), base::UTF16ToUTF8(name),
          base::UTF16ToUTF8(summary)));
    }
  }
  std::move(callback).Run(std::move(addresses));
}

void SettingsWindowHandler::RemoveAddress(const std::string& guid) {
  if (autofill::PersonalDataManager* data = GetPersonalDataManager()) {
    data->address_data_manager().RemoveProfile(guid);
  }
}

void SettingsWindowHandler::OnPersonalDataChanged() {
  page_->AutofillChanged();
}

}  // namespace compass
