# Security policy

## Reporting a vulnerability

Please don't open a public issue for security problems. Report them
privately through GitHub: on this repository's **Security** tab, choose
**Report a vulnerability**.

Include what you found, how to reproduce it, and the versions affected. We'll
acknowledge the report within a few days, keep you informed while we fix it,
and credit you in the release notes if you'd like.

## Scope

In scope:

- Compass's own code in this repository: the desktop browser's `//compass`
  code, `chromium_src` overrides and patches; the iOS app; the sync server
  (`server/`), including the pairing protocol and end-to-end encryption; the
  update feeds and the Windows updater's verification of installers.
- The public services: `updates.compassbrowser.org` and the project's public
  sync server.

Vulnerabilities in Chromium itself are best reported to the
[Chromium project](https://www.chromium.org/Home/chromium-security/reporting-security-bugs/).
Tell us too if you think Compass is affected differently, or if a fix isn't in
the Chromium version Compass is on yet.

## Supported versions

Only the latest stable and beta releases get security fixes. Compass
follows Chromium's stable channel; `config/chromium_version` shows the
version a build is based on.
