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
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/autofill/personal_data_manager_factory.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/extensions/api/passwords_private/passwords_private_delegate_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/ui/webui/settings/settings_window_handler.h"
#include "components/autofill/core/browser/data_manager/addresses/address_data_manager.h"
#include "components/autofill/core/browser/data_manager/payments/payments_data_manager.h"
#include "components/autofill/core/browser/data_model/addresses/autofill_profile.h"
#include "components/autofill/core/browser/data_model/payments/credit_card.h"
#include "components/autofill/core/browser/data_quality/validation.h"
#include "components/autofill/core/browser/field_types.h"
#include "extensions/browser/extension_registry.h"

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

// Passwords -------------------------------------------------------------------

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

// Safety check ------------------------------------------------------------

void SettingsWindowHandler::GetSafetyCheck(GetSafetyCheckCallback callback) {
  auto result = sidebar::mojom::SafetyCheckResult::New();
  if (extensions::ExtensionRegistry* registry =
          extensions::ExtensionRegistry::Get(profile_)) {
    result->blocklisted_extensions =
        static_cast<int>(registry->blocklisted_extensions().size());
    result->enabled_extensions =
        static_cast<int>(registry->enabled_extensions().size());
  }
  extensions::PasswordsPrivateDelegate* delegate = GetPasswordsDelegate();
  if (!delegate) {
    std::move(callback).Run(std::move(result));
    return;
  }
  // Once the passwords are loaded, what the password manager knows of them.
  delegate->GetSavedPasswordsList(base::BindOnce(
      [](base::WeakPtr<SettingsWindowHandler> handler,
         sidebar::mojom::SafetyCheckResultPtr result,
         GetSafetyCheckCallback callback,
         const extensions::PasswordsPrivateDelegate::UiEntries& entries) {
        result->saved_passwords = static_cast<int>(entries.size());
        if (handler && handler->passwords_delegate_) {
          for (const passwords_private::PasswordUiEntry& entry :
               handler->passwords_delegate_->GetInsecureCredentials()) {
            if (!entry.compromised_info || entry.compromised_info->is_muted) {
              continue;
            }
            bool compromised = false;
            for (passwords_private::CompromiseType type :
                 entry.compromised_info->compromise_types) {
              switch (type) {
                case passwords_private::CompromiseType::kLeaked:
                case passwords_private::CompromiseType::kPhished:
                  compromised = true;
                  break;
                case passwords_private::CompromiseType::kWeak:
                  result->weak_passwords++;
                  break;
                case passwords_private::CompromiseType::kReused:
                  result->reused_passwords++;
                  break;
                default:
                  break;
              }
            }
            if (compromised) {
              result->compromised_passwords++;
            }
          }
        }
        std::move(callback).Run(std::move(result));
      },
      weak_factory_.GetWeakPtr(), std::move(result), std::move(callback)));
}

// Payment cards and addresses -------------------------------------------------

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

// The card and address editors ------------------------------------------------

void SettingsWindowHandler::GetCard(const std::string& guid,
                                    GetCardCallback callback) {
  autofill::PersonalDataManager* data = GetPersonalDataManager();
  const autofill::CreditCard* card =
      data ? data->payments_data_manager().GetCreditCardByGUID(guid) : nullptr;
  // Only Compass's own cards are edited here; account ones in the account.
  if (!card || !autofill::CreditCard::IsLocalCard(card)) {
    std::move(callback).Run(nullptr);
    return;
  }
  std::move(callback).Run(sidebar::mojom::CardDraft::New(
      card->guid(),
      base::UTF16ToUTF8(card->GetRawInfo(autofill::CREDIT_CARD_NAME_FULL)),
      std::string(), card->expiration_month(), card->expiration_year(),
      base::UTF16ToUTF8(card->nickname()),
      base::UTF16ToUTF8(card->LastFourDigits())));
}

void SettingsWindowHandler::SaveCard(sidebar::mojom::CardDraftPtr draft,
                                     SaveCardCallback callback) {
  autofill::PersonalDataManager* data = GetPersonalDataManager();
  if (!data) {
    std::move(callback).Run("Payment methods aren't available here.");
    return;
  }
  autofill::PaymentsDataManager& payments = data->payments_data_manager();
  autofill::CreditCard card;
  if (!draft->guid.empty()) {
    const autofill::CreditCard* existing =
        payments.GetCreditCardByGUID(draft->guid);
    if (!existing || !autofill::CreditCard::IsLocalCard(existing)) {
      std::move(callback).Run("This card can't be edited here.");
      return;
    }
    card = *existing;
  } else {
    std::u16string number;
    base::RemoveChars(base::UTF8ToUTF16(draft->number), u" -", &number);
    card.SetNumber(number);
    if (!card.HasValidCardNumber()) {
      std::move(callback).Run("That card number isn't valid.");
      return;
    }
  }
  if (!autofill::IsValidCreditCardExpirationDate(
          draft->expiration_year, draft->expiration_month, base::Time::Now())) {
    std::move(callback).Run("Use an expiry date that hasn't passed.");
    return;
  }
  card.SetRawInfo(autofill::CREDIT_CARD_NAME_FULL,
                  base::UTF8ToUTF16(draft->name_on_card));
  card.SetExpirationMonth(draft->expiration_month);
  card.SetExpirationYear(draft->expiration_year);
  card.SetNickname(base::UTF8ToUTF16(draft->nickname));
  if (draft->guid.empty()) {
    payments.AddCreditCard(card);
  } else {
    payments.UpdateCreditCard(card);
  }
  std::move(callback).Run(std::string());
}

void SettingsWindowHandler::GetAddress(const std::string& guid,
                                       GetAddressCallback callback) {
  autofill::PersonalDataManager* data = GetPersonalDataManager();
  const autofill::AutofillProfile* profile =
      data ? data->address_data_manager().GetProfileByGUID(guid) : nullptr;
  if (!profile) {
    std::move(callback).Run(nullptr);
    return;
  }
  auto raw = [&](autofill::FieldType type) {
    return base::UTF16ToUTF8(profile->GetRawInfo(type));
  };
  std::move(callback).Run(sidebar::mojom::AddressDraft::New(
      profile->guid(), raw(autofill::NAME_FULL), raw(autofill::COMPANY_NAME),
      raw(autofill::ADDRESS_HOME_STREET_ADDRESS),
      raw(autofill::ADDRESS_HOME_CITY), raw(autofill::ADDRESS_HOME_STATE),
      raw(autofill::ADDRESS_HOME_ZIP), raw(autofill::ADDRESS_HOME_COUNTRY),
      raw(autofill::PHONE_HOME_WHOLE_NUMBER), raw(autofill::EMAIL_ADDRESS)));
}

void SettingsWindowHandler::SaveAddress(sidebar::mojom::AddressDraftPtr draft,
                                        SaveAddressCallback callback) {
  autofill::PersonalDataManager* data = GetPersonalDataManager();
  if (!data) {
    std::move(callback).Run("Addresses aren't available here.");
    return;
  }
  autofill::AddressDataManager& addresses = data->address_data_manager();
  const std::string country = base::ToUpperASCII(draft->country_code);
  if (country.size() != 2 || !base::IsAsciiAlpha(country[0]) ||
      !base::IsAsciiAlpha(country[1])) {
    std::move(callback).Run("Pick a country.");
    return;
  }
  autofill::AutofillProfile profile{autofill::AddressCountryCode(country)};
  if (!draft->guid.empty()) {
    const autofill::AutofillProfile* existing =
        addresses.GetProfileByGUID(draft->guid);
    if (!existing) {
      std::move(callback).Run("This address was removed.");
      return;
    }
    profile = *existing;
  }
  if (draft->full_name.empty() && draft->street.empty() &&
      draft->email.empty() && draft->phone.empty()) {
    std::move(callback).Run("Fill in at least a name or a street.");
    return;
  }
  const std::string locale = g_browser_process->GetApplicationLocale();
  const std::pair<autofill::FieldType, const std::string*> fields[] = {
      {autofill::ADDRESS_HOME_COUNTRY, &country},
      {autofill::NAME_FULL, &draft->full_name},
      {autofill::COMPANY_NAME, &draft->organization},
      {autofill::ADDRESS_HOME_STREET_ADDRESS, &draft->street},
      {autofill::ADDRESS_HOME_CITY, &draft->city},
      {autofill::ADDRESS_HOME_STATE, &draft->state},
      {autofill::ADDRESS_HOME_ZIP, &draft->postal_code},
      {autofill::PHONE_HOME_WHOLE_NUMBER, &draft->phone},
      {autofill::EMAIL_ADDRESS, &draft->email},
  };
  for (const auto& [type, value] : fields) {
    profile.SetInfoWithVerificationStatus(
        type, base::UTF8ToUTF16(*value), locale,
        autofill::VerificationStatus::kUserVerified);
  }
  profile.FinalizeAfterImport();
  if (draft->guid.empty()) {
    addresses.AddProfile(profile);
  } else {
    addresses.UpdateProfile(profile);
  }
  std::move(callback).Run(std::string());
}

void SettingsWindowHandler::OnPersonalDataChanged() {
  page_->AutofillChanged();
}

}  // namespace compass
