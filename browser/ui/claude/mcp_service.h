// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_MCP_SERVICE_H_
#define COMPASS_BROWSER_UI_CLAUDE_MCP_SERVICE_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/pref_change_registrar.h"

class Profile;

namespace compass::claude {

// Tells the MCP bridge whether its profile allows MCP clients, as the
// setting changes, and lets go as the profile goes (docs/design/claude.md,
// D6). Made with every regular profile.
class McpService : public KeyedService {
 public:
  explicit McpService(Profile* profile);
  McpService(const McpService&) = delete;
  McpService& operator=(const McpService&) = delete;
  ~McpService() override;

  // KeyedService:
  void Shutdown() override;

 private:
  void Update();

  const raw_ptr<Profile> profile_;
  PrefChangeRegistrar registrar_;
};

class McpServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static McpServiceFactory* GetInstance();

  McpServiceFactory(const McpServiceFactory&) = delete;
  McpServiceFactory& operator=(const McpServiceFactory&) = delete;

 private:
  friend base::NoDestructor<McpServiceFactory>;

  McpServiceFactory();
  ~McpServiceFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  // Tests drive McpBridge themselves.
  bool ServiceIsNULLWhileTesting() const override;
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_CLAUDE_MCP_SERVICE_H_
