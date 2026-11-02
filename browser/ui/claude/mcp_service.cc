// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/mcp_service.h"

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/ui/claude/mcp_bridge.h"
#include "components/prefs/pref_service.h"

namespace compass::claude {

McpService::McpService(Profile* profile) : profile_(profile) {
  registrar_.Init(profile->GetPrefs());
  registrar_.Add(prefs::kMcpEnabled, base::BindRepeating(&McpService::Update,
                                                         base::Unretained(this)));
  Update();
}

McpService::~McpService() = default;

void McpService::Shutdown() {
  registrar_.RemoveAll();
  if (McpBridge* bridge = McpBridge::GetIfExists()) {
    bridge->SetProfileEnabled(profile_, false);
  }
}

void McpService::Update() {
  const bool enabled = profile_->GetPrefs()->GetBoolean(prefs::kMcpEnabled);
  // Nothing to make while it's off everywhere.
  if (enabled || McpBridge::GetIfExists()) {
    McpBridge::Get()->SetProfileEnabled(profile_, enabled);
  }
}

// static
McpServiceFactory* McpServiceFactory::GetInstance() {
  static base::NoDestructor<McpServiceFactory> instance;
  return instance.get();
}

McpServiceFactory::McpServiceFactory()
    : ProfileKeyedServiceFactory(
          "CompassMcpService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {}

McpServiceFactory::~McpServiceFactory() = default;

std::unique_ptr<KeyedService>
McpServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<McpService>(Profile::FromBrowserContext(context));
}

bool McpServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

bool McpServiceFactory::ServiceIsNULLWhileTesting() const {
  return true;
}

}  // namespace compass::claude
