// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compass keeps Manifest V2 extensions working
// (chromium_src/extensions/browser/manifest_v2_util.cc). These tests run with
// upstream's testing bypass off, so they see what users get.
//
// They replace upstream's manifest_v2_util_unittest.cc and
// manifest_v2_handler_unittest.cc, which test the deprecation Compass turns off
// and fail against the override. Compass doesn't build upstream's unit_tests.

#include "base/memory/raw_ptr.h"
#include "chrome/browser/extensions/extension_service_user_test_base.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registrar.h"
#include "extensions/browser/manifest_v2_handler.h"
#include "extensions/browser/manifest_v2_util.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_builder.h"
#include "extensions/common/manifest.h"
#include "extensions/common/mojom/manifest.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

using extensions::Extension;
using extensions::ExtensionBuilder;
using extensions::ManifestV2Handler;
using extensions::mojom::ManifestLocation;

// Every location a user's extension can come from, upstream's "affected" ones.
constexpr struct {
  ManifestLocation location;
  const char* name;
} kLocations[] = {
    {ManifestLocation::kInternal, "internal (store or .crx)"},
    {ManifestLocation::kUnpacked, "unpacked"},
    {ManifestLocation::kCommandLine, "command line"},
    {ManifestLocation::kExternalPref, "external pref"},
    {ManifestLocation::kExternalPrefDownload, "external pref download"},
    {ManifestLocation::kExternalRegistry, "external registry"},
    {ManifestLocation::kExternalPolicy, "policy"},
    {ManifestLocation::kExternalPolicyDownload, "policy download"},
};

class CompassManifestV2Test : public extensions::ExtensionServiceUserTestBase {
 protected:
  void SetUp() override {
    ExtensionServiceUserTestBase::SetUp();
    InitializeExtensionService(ExtensionServiceInitParams{});
    handler_ = ManifestV2Handler::Get(profile());
    ASSERT_TRUE(handler_);
  }

  void TearDown() override {
    handler_ = nullptr;
    ExtensionServiceUserTestBase::TearDown();
  }

  // Upstream's tests allow MV2 through a testing switch; Compass must allow it
  // without one.
  bool ShouldAllowMV2Extensions() override { return false; }

  ManifestV2Handler* handler() { return handler_.get(); }

 private:
  raw_ptr<ManifestV2Handler> handler_;
};

TEST_F(CompassManifestV2Test, NoMV2ExtensionIsAffected) {
  for (const auto& test_case : kLocations) {
    SCOPED_TRACE(test_case.name);
    scoped_refptr<const Extension> extension =
        ExtensionBuilder(test_case.name)
            .SetManifestVersion(2)
            .SetLocation(test_case.location)
            .Build();
    EXPECT_FALSE(extensions::manifest_v2_util::IsExtensionAffected(*extension));
    EXPECT_FALSE(handler()->IsExtensionAffected(*extension));
  }
}

TEST_F(CompassManifestV2Test, MV2ExtensionsInstall) {
  for (const auto& test_case : kLocations) {
    SCOPED_TRACE(test_case.name);
    EXPECT_FALSE(handler()->ShouldBlockExtensionInstallation(
        2, extensions::Manifest::Type::kExtension, test_case.location));
  }
  EXPECT_FALSE(handler()->ShouldBlockExtensionInstallation(
      2, extensions::Manifest::Type::kUserScript, ManifestLocation::kInternal));
}

TEST_F(CompassManifestV2Test, MV2ExtensionsEnable) {
  for (const auto& test_case : kLocations) {
    SCOPED_TRACE(test_case.name);
    scoped_refptr<const Extension> extension =
        ExtensionBuilder(test_case.name)
            .SetManifestVersion(2)
            .SetLocation(test_case.location)
            .Build();
    EXPECT_FALSE(handler()->ShouldBlockExtensionEnable(*extension));
  }
}

// Upstream disables every MV2 extension when the extension system is ready.
TEST_F(CompassManifestV2Test, MV2ExtensionsStayEnabledAtStartup) {
  scoped_refptr<const Extension> extension =
      ExtensionBuilder("MV2 extension")
          .SetManifestVersion(2)
          .SetLocation(ManifestLocation::kInternal)
          .Build();
  registrar()->AddExtension(extension.get());
  ASSERT_TRUE(registry()->enabled_extensions().Contains(extension->id()));

  handler()->DisableAffectedExtensionsForTesting();

  EXPECT_TRUE(registry()->enabled_extensions().Contains(extension->id()));
}

}  // namespace
}  // namespace compass
