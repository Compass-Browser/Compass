// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/ui/external_layer/external_layer_registry.h"

#include <memory>

#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "compass/ui/external_layer/external_layer_registry_impl.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

TEST(ExternalLayerRegistryTest, RegisterLookupUnregister) {
  ExternalLayerRegistry& registry = ExternalLayerRegistry::Get();
  auto id = base::UnguessableToken::Create();
  EXPECT_FALSE(registry.Lookup(id));

  registry.Register(id, 42);
  EXPECT_EQ(registry.Lookup(id), 42u);

  registry.Register(id, 43);
  EXPECT_EQ(registry.Lookup(id), 43u);

  registry.Unregister(id);
  EXPECT_FALSE(registry.Lookup(id));
}

// Contexts registered by the browser are dropped when its connection closes
// (e.g. the browser crashed), so stale plane ids never host anything.
TEST(ExternalLayerRegistryTest, DropsRegistrationsWhenConnectionCloses) {
  base::test::SingleThreadTaskEnvironment task_environment;
  auto id = base::UnguessableToken::Create();
  {
    mojo::Remote<mojom::ExternalLayerRegistry> remote;
    BindExternalLayerRegistry(remote.BindNewPipeAndPassReceiver());
    remote->Register(id, 7);
    remote.FlushForTesting();
    EXPECT_EQ(ExternalLayerRegistry::Get().Lookup(id), 7u);
  }
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(ExternalLayerRegistry::Get().Lookup(id));
}

}  // namespace
}  // namespace compass
