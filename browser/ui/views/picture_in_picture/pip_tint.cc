// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/picture_in_picture/pip_tint.h"

#include <utility>

#include "base/no_destructor.h"

namespace compass {

namespace {

PipTintProvider& GetProvider() {
  static base::NoDestructor<PipTintProvider> provider;
  return *provider;
}

}  // namespace

void SetPipTintProvider(PipTintProvider provider) {
  GetProvider() = std::move(provider);
}

std::optional<SkColor> GetPipTint(content::WebContents* contents) {
  const PipTintProvider& provider = GetProvider();
  return provider.is_null() ? std::nullopt : provider.Run(contents);
}

}  // namespace compass
