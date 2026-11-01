// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_NATIVE_MESSAGING_CHROME_HOSTS_MAC_H_
#define COMPASS_BROWSER_NATIVE_MESSAGING_CHROME_HOSTS_MAC_H_

namespace base {
class FilePath;
}  // namespace base

namespace compass {

// Apps like 1Password write their native messaging host manifest only into
// the folders of browsers they know, never Compass's. 1Password writes it
// into Chrome's folder (~/Library/Application Support/Google/Chrome/
// NativeMessagingHosts) even when Chrome isn't installed. At each launch this
// copies the manifests of the hosts Compass takes from there
// (com.1password.1password) into |compass_dir|, Compass's own
// NativeMessagingHosts folder, when Compass's copy is missing or differs, so
// an app update that changes its manifest reaches Compass too. A manifest
// missing from Chrome's folder leaves Compass's copy as it is.
void CopyNativeMessagingHostsFromChrome(const base::FilePath& chrome_dir,
                                        const base::FilePath& compass_dir);

// The above, from this user's Chrome folder.
void CopyNativeMessagingHostsFromChrome(const base::FilePath& compass_dir);

}  // namespace compass

#endif  // COMPASS_BROWSER_NATIVE_MESSAGING_CHROME_HOSTS_MAC_H_
