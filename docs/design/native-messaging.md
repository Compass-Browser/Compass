# Native messaging hosts

Extensions such as 1Password talk to their desktop app through a native
messaging host: a JSON manifest, in the browser's `NativeMessagingHosts`
folder, naming the helper program to launch and the extension IDs allowed to
use it. Compass looks in `~/Library/Application Support/Compass/
NativeMessagingHosts` (upstream's `chrome::DIR_USER_NATIVE_MESSAGING`), then
in `/Library/Application Support/Chromium/NativeMessagingHosts`.

Apps only write their manifest into the folders of browsers they know, and
Compass isn't one of them. 1Password writes it for Chrome, Edge, Vivaldi,
Arc, Firefox and others, whether or not those browsers are installed; it
doesn't write one for plain Chromium either.

## macOS

At each launch, right after creating its `NativeMessagingHosts` folder,
Compass copies the manifests of the hosts it takes from Chrome
(`com.1password.1password`, list in
`browser/native_messaging/chrome_hosts_mac.cc`) from
`~/Library/Application Support/Google/Chrome/NativeMessagingHosts`, when its
own copy is missing or differs. Copying keeps the app's own helper path and
extension IDs, and picks up the app's updates. A manifest missing from
Chrome's folder leaves Compass's copy as it is. Chrome doesn't need to be
installed.

1Password's helper also checks the browser's code signature before it talks
to the desktop app. Compass, signed with its own Developer ID, passes (checked
with 1Password 8.12.42 on 2026-11-01); if it ever doesn't, the reason is in
1Password's `BrowserSupport` log
(`~/Library/Group Containers/2BUA8C4S2C.com.1password/Library/Application
Support/1Password/Data/logs/BrowserSupport/`).

## Windows

Nothing to copy. There the hosts are registry keys whose default value is the
manifest's path, and Compass is a Chromium-branded build, so upstream's
`launch_context_win.cc` already looks in
`Software\Chromium\NativeMessagingHosts` and then in
`Software\Google\Chrome\NativeMessagingHosts`, under HKCU and HKLM (32-bit
view first, then 64-bit). `kProductPathName` ("Compass") plays no part in it.
If 1Password writes Chrome's key, as it writes Chrome's folder on macOS,
Compass finds the manifest as it is.

What can still stop it is 1Password's own check of the browser. On Windows its
browser support helper checks the Authenticode publisher of
the process that launched it and refuses ones it doesn't trust. An unsigned
Compass shows up in its log (`%LOCALAPPDATA%\1Password\logs\BrowserSupport`)
as "Unknown Publisher" or "no signature was present in the subject". Community
posts say newer 1Password builds let the user add a browser as trusted; that
isn't confirmed. If neither works, Compass needs an Authenticode certificate,
which is the user's call.

Not yet checked on a Windows machine. To check, with 1Password and Compass
installed:

1. `reg query "HKCU\Software\Google\Chrome\NativeMessagingHosts\com.1password.1password" /ve`
   (and HKLM) shows the manifest's path.
2. Unlock 1Password from the extension in Compass; if it doesn't reach the
   app, read the newest BrowserSupport log.
