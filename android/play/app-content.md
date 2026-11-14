# App content (draft): content rating and the other declarations

Play Console's App content page, besides Data safety (data-safety.md). Like
that one, a draft for the developer to check.

## Content rating (IARC questionnaire)

- **Category:** All other app types (Utility, Productivity, Communication, or
  Other). A browser isn't a game, a social app or a reference app.
- **Violence, fear, sexuality, language, controlled substances, gambling,
  crude humor:** No to every question. The app has no content of its own;
  what websites show is the web's.
- **Does the app allow users to interact or exchange content with other
  users?** No. Sync and sent tabs only reach the user's own devices.
- **Does the app share the user's current physical location with other
  users?** No.
- **Does the app allow users to purchase digital goods?** No.
- **Is the app a web browser or search engine, or does it give unrestricted
  internet access?** Yes. (IARC notes "Unrestricted Internet"; browsers get
  a low age rating with that note, as Chrome and Firefox do.)
- **Does the app contain or generate AI content?** Where asked: Yes, Claude
  (text), only with the user's own key or endpoint, about the page the user
  is on.

## Other declarations

- **Privacy policy:** https://compassbrowser.org/privacy (to update for
  Android first: data-safety.md, "Judgment calls").
- **Ads:** No, the app contains no ads.
- **App access:** All functionality is available without special access,
  except sync (it pairs with Compass on another device) and Claude (it needs
  the user's own Claude API key). Reviewer note: "Sync needs Compass on a
  second device and Claude the user's own API key; both are optional. The
  browser works without either."
- **Target audience and content:** 18 and over (or 13 and over); not
  designed for children, not in Designed for Families.
- **News app:** No. **Government app:** No. **Financial features:** none.
  **Health:** none.
- **Advertising ID:** No, the app doesn't use it (no
  `com.google.android.gms.permission.AD_ID` in the manifest;
  permissions.md).
- **Sensitive permissions and foreground services:** permissions.md lists
  what the manifest asks for and the declarations Play wants for them.
