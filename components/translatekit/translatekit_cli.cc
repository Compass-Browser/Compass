// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// translatekit_cli <packages> <from> <to> [--html] [--workers N]
//
// Translates stdin with the engine the library wraps, for trying models and
// timing a machine. <packages> holds one folder per package, named
// "<language1>-<language2>", each with its direction folders (engine.h).
// Prints the translation to stdout and the time it took to stderr.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "compass/components/translatekit/disk_file_source.h"
#include "compass/components/translatekit/engine.h"

namespace {

using compass::translatekit::DiskFileSource;
using compass::translatekit::Engine;
using compass::translatekit::Package;

int Usage() {
  std::fprintf(stderr,
               "usage: translatekit_cli <packages> <from> <to> [--html] "
               "[--workers N]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    return Usage();
  }
  bool html = false;
  size_t workers = 0;
  for (int i = 4; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--html")) {
      html = true;
    } else if (!std::strcmp(argv[i], "--workers") && i + 1 < argc) {
      workers = static_cast<size_t>(std::atoi(argv[++i]));
    } else {
      return Usage();
    }
  }

  std::vector<Package> packages;
  std::error_code error;
  for (const auto& entry :
       std::filesystem::directory_iterator(argv[1], error)) {
    if (!entry.is_directory()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    const size_t dash = name.find('-');
    if (dash == std::string::npos) {
      continue;
    }
    packages.push_back({name.substr(0, dash), name.substr(dash + 1),
                        entry.path().generic_string()});
  }
  if (error || packages.empty()) {
    std::fprintf(stderr, "no packages in %s\n", argv[1]);
    return 1;
  }

  const auto start = std::chrono::steady_clock::now();
  Engine engine(std::make_unique<DiskFileSource>(), workers);
  engine.SetPackages(std::move(packages));
  auto translator = engine.CreateTranslator(argv[2], argv[3]);
  if (!translator) {
    std::fprintf(stderr, "can't translate %s to %s with these packages\n",
                 argv[2], argv[3]);
    return 1;
  }
  const auto loaded = std::chrono::steady_clock::now();

  std::string input((std::istreambuf_iterator<char>(std::cin)),
                    std::istreambuf_iterator<char>());
  auto output = translator->Translate(input, html);
  const auto done = std::chrono::steady_clock::now();
  if (!output) {
    std::fprintf(stderr, "translation failed\n");
    return 1;
  }
  std::cout << *output << std::endl;

  using Ms = std::chrono::duration<double, std::milli>;
  std::fprintf(stderr, "workers %zu, load %.0f ms, translate %.0f ms\n",
               workers ? workers : Engine::DefaultWorkers(),
               Ms(loaded - start).count(), Ms(done - loaded).count());
  return 0;
}
