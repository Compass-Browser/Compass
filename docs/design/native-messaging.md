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
to the desktop app; a browser 1Password doesn't trust is refused there, with
the reason in 1Password's `BrowserSupport` log
(`~/Library/Group Containers/2BUA8C4S2C.com.1password/Library/Application
Support/1Password/Data/logs/BrowserSupport/`).

## Windows

Not done yet. There the hosts are registry keys
(`HKCU\Software\Google\Chrome\NativeMessagingHosts\<host>`, whose default
value is the manifest's path), not files.
