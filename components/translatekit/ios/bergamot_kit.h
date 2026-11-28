// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_TRANSLATEKIT_IOS_BERGAMOT_KIT_H_
#define COMPASS_COMPONENTS_TRANSLATEKIT_IOS_BERGAMOT_KIT_H_

// The engine (engine.h) for iOS, as plain C for Swift: BergamotKit.xcframework
// (tools/translation/build_ios.py), wrapped by CompassKit's
// CompassTranslate (docs/design/translation.md, "iOS"). The TranslateKit
// ABI the desktop's service loads isn't needed there.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BKEngine BKEngine;
typedef struct BKTranslator BKTranslator;

// `workers`: translation threads; 0 picks from the device's cores.
BKEngine* bk_engine_create(size_t workers);
void bk_engine_destroy(BKEngine* engine);

// The installed packages: folders named "<language1>-<language2>" holding a
// direction's model.bin and vocabularies, as on the desktop. Replaces the
// previous list.
void bk_engine_set_packages(BKEngine* engine,
                            const char* const* languages1,
                            const char* const* languages2,
                            const char* const* paths,
                            size_t count);

// NULL when no package translates `source` into `target` (directly or
// through English), or a model doesn't load.
BKTranslator* bk_translator_create(BKEngine* engine,
                                   const char* source,
                                   const char* target);
void bk_translator_destroy(BKTranslator* translator);

// `html`: `text` is HTML, its tags kept with their words. A NUL-terminated
// UTF-8 string to free with bk_free(), or NULL on failure. Thread-safe;
// blocks until done.
char* bk_translate(BKTranslator* translator,
                   const char* text,
                   size_t length,
                   int html);
void bk_free(char* text);

// Decompresses the zstd file at `source` into `destination` (the mirror's
// models are zstd, which Apple's Compression framework doesn't read), a
// chunk at a time. 1 on success, 0 on failure (destination may be partly
// written).
int bk_zstd_decompress_file(const char* source, const char* destination);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // COMPASS_COMPONENTS_TRANSLATEKIT_IOS_BERGAMOT_KIT_H_
