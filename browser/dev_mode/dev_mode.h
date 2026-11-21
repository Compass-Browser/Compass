// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_DEV_MODE_DEV_MODE_H_
#define COMPASS_BROWSER_DEV_MODE_DEV_MODE_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

class GURL;
class PrefService;

namespace content {
class WebContents;
}

// Development mode: per-site tools for web developers (docs/design/dev-mode.md).
// The sites are host patterns in prefs::kDevModeHosts.
namespace compass::dev_mode {

// The pattern to keep for what the user typed or for a page's host, or
// nothing for what isn't a web host. Lenient: a scheme, a port and a path
// are dropped ("http://localhost:3000/app" is "localhost"), "www." is the
// site itself (BoostMatch), and a leading "*." keeps the subdomains.
std::optional<std::string> NormalizeHost(std::string_view input);

// Whether development mode can be on for a page at |url|: http and https.
bool CanEnable(const GURL& url);

// Whether a page at |url| is in development mode.
bool IsEnabledFor(const PrefService* prefs, const GURL& url);

// Turns development mode on for |url|'s host, or off for every pattern
// that matches |url| (so turning it off on a page always takes it off
// there, even when "*.example.com" put it on).
void SetEnabledFor(PrefService* prefs, const GURL& url, bool enabled);

// The patterns, in the order they were added (the settings page's list).
std::vector<std::string> Hosts(const PrefService* prefs);

// Adds |input| as NormalizeHost() writes it, unless it's there already.
// The pattern kept, or nothing when |input| isn't a host.
std::optional<std::string> AddHost(PrefService* prefs, std::string_view input);

// Removes the pattern |pattern| (as Hosts() lists it).
void RemoveHost(PrefService* prefs, std::string_view pattern);

// Whether |contents|' committed page is in development mode, for its
// profile.
bool IsEnabledFor(content::WebContents* contents);

// Whether reloading |contents| skips the cache: its page is in development
// mode. Called by upstream's chrome::Reload() (see
// patches/chrome-browser-ui-browser_commands.cc.patch).
bool ShouldBypassCache(content::WebContents* contents);

}  // namespace compass::dev_mode

#endif  // COMPASS_BROWSER_DEV_MODE_DEV_MODE_H_
