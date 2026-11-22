// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CHATGPT_ACCOUNT_H_
#define COMPASS_BROWSER_CLAUDE_CHATGPT_ACCOUNT_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "compass/browser/claude/chatgpt_oauth.h"
#include "url/gurl.h"

class PrefService;
class Profile;

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace os_crypt_async {
class Encryptor;
}  // namespace os_crypt_async

namespace compass::claude {

// A profile's ChatGPT sign-in (docs/design/ai-providers.md, "Sign in with
// ChatGPT"): signs in through the browser and a loopback redirect, keeps the
// tokens encrypted (prefs::kChatGPTSession, like the API key), and hands out
// a fresh access token, refreshing it when it's close to expiring. While
// signed in, the OpenAI provider uses it instead of the key
// (ClaudeCredentials::GetApiKey()). Lives as long as the profile.
class ChatGPTAccount : public base::SupportsUserData::Data {
 public:
  // Opens the authorize page in the browser.
  using OpenUrlCallback = base::OnceCallback<void(const GURL&)>;
  // A sign-in ended: nullopt when signed in, else why not, for the user.
  using SignInCallback =
      base::OnceCallback<void(std::optional<std::string> error)>;
  // The access token, or what went wrong, for the user.
  using TokenCallback =
      base::OnceCallback<void(base::expected<std::string, std::string>)>;

  static ChatGPTAccount* ForProfile(Profile* profile);

  // Tokens are saved (they may still turn out refused).
  static bool IsSignedIn(const PrefService& prefs);
  // The signed-in account's email; empty when signed out.
  static std::string AccountEmail(const PrefService& prefs);

  explicit ChatGPTAccount(Profile* profile);
  ChatGPTAccount(const ChatGPTAccount&) = delete;
  ChatGPTAccount& operator=(const ChatGPTAccount&) = delete;
  ~ChatGPTAccount() override;

  // Starts a sign-in: listens on 127.0.0.1, then |open_url| opens the
  // authorize page; |done| runs when the redirect comes back and the tokens
  // are saved, or the sign-in fails, is cancelled or times out. Another one
  // underway is cancelled first.
  void SignIn(OpenUrlCallback open_url, SignInCallback done);
  void CancelSignIn();
  bool IsSigningIn() const { return !!pending_; }

  // Forgets the tokens (now), then asks OpenAI to revoke them; |done| tells
  // whether it confirmed. Compass's registration stays, for the next
  // sign-in.
  void SignOut(base::OnceCallback<void(bool revoked)> done);

  // A fresh access token: the saved one, or a refreshed one. Refreshes are
  // one at a time (the refresh token rotates). A refused refresh signs out.
  void GetAccessToken(TokenCallback callback);

  // Why the last sign-in ended by itself (a refused refresh), until the next
  // sign-in; empty if it didn't.
  const std::string& ended_reason() const { return ended_reason_; }
  // What the last GetAccessToken() failed with, until one succeeds; empty if
  // it didn't.
  const std::string& last_token_error() const { return last_token_error_; }

  // Tests: where the token, discovery and authorize requests go, and what
  // they're sent with.
  void SetUrlsForTesting(const GURL& authorize_url,
                         const GURL& token_url,
                         const GURL& discovery_url);
  void SetLoaderFactoryForTesting(
      scoped_refptr<network::SharedURLLoaderFactory> factory);

 private:
  class CallbackServer;
  struct PendingSignIn;

  scoped_refptr<network::SharedURLLoaderFactory> LoaderFactory();
  std::unique_ptr<network::SimpleURLLoader> MakePost(const GURL& url,
                                                     const std::string& body);

  // Sign-in.
  void OnServerStarted(OpenUrlCallback open_url, int port);
  void OnCallbackRequest(int connection_id, const GURL& url);
  void OnCodeExchanged(std::optional<std::string> body);
  void OnSignInStored(chatgpt::Session session, bool stored);
  // Answers the browser's request, if one is waiting, and ends the sign-in.
  void FinishSignIn(std::optional<std::string> error);

  // The tokens.
  void LoadSession(base::OnceCallback<void(bool)> callback);
  void OnEncryptorForLoad(base::OnceCallback<void(bool)> callback,
                          scoped_refptr<os_crypt_async::Encryptor> encryptor);
  void StoreSession(const chatgpt::Session& session,
                    base::OnceCallback<void(bool)> callback);
  void ClearSession();
  void OnLoadedForToken(bool loaded);
  void OnRefreshed(int generation, std::optional<std::string> body);
  void ResolveWaiting(base::expected<std::string, std::string> result);

  // Sign-out.
  void OnLoadedForSignOut(base::OnceCallback<void(bool)> done, bool loaded);
  void Revoke(std::string client_id,
              std::string refresh_token,
              base::OnceCallback<void(bool)> done);
  void OnDiscovery(std::string client_id,
                   std::string refresh_token,
                   base::OnceCallback<void(bool)> done,
                   std::optional<std::string> body);
  void OnRevoked(base::OnceCallback<void(bool)> done,
                 std::optional<std::string> body);

  const raw_ptr<Profile> profile_;
  GURL authorize_url_;
  GURL token_url_;
  GURL discovery_url_;
  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_for_testing_;

  std::unique_ptr<PendingSignIn> pending_;

  // The tokens, once read (decrypting takes a moment).
  std::optional<chatgpt::Session> session_;
  // Waiting for a token, while one is read or refreshed.
  std::vector<TokenCallback> waiting_;
  bool getting_token_ = false;
  std::unique_ptr<network::SimpleURLLoader> refresh_loader_;
  // Goes up on each sign-in and sign-out: a refresh that started before
  // doesn't save its tokens over theirs.
  int generation_ = 0;
  std::string ended_reason_;
  std::string last_token_error_;

  std::unique_ptr<network::SimpleURLLoader> revoke_loader_;

  base::WeakPtrFactory<ChatGPTAccount> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CHATGPT_ACCOUNT_H_
