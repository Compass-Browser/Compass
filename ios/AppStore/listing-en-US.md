# App Store listing (en-US)

What App Store Connect asks for, for Compass Browser (`it.cipi1965.compass`,
iPhone and iPad). The Play listing (`android/play/listing-en-US.md`) is the
Android counterpart; this one describes the iOS app (docs/design/ios.md).

## Name (30 characters at most)

```
Compass Browser
```

"Compass" alone is taken on the App Store (ITMS-90129).

## Subtitle (30 characters at most)

```
Spaces, private sync, Claude
```

## Promotional text (170 characters at most; changes without a review)

```
Spaces keep work, home and projects apart. Sync with your Mac, PC and Android, end-to-end encrypted and without an account. Ask Claude about any page.
```

## Description (4000 characters at most)

```
Compass is a browser that keeps your tabs in order. It's the Compass you may use on your Mac, Windows PC or Android device, on iPhone and iPad.

SPACES
• Keep work, home and projects apart: each space has its own pinned tabs, folders, colors and theme.
• Pin the tabs you come back to. Favorites stay at the top of every space.
• Today holds the tabs you open along the way, and tidies them into the archive after a while, so the sidebar doesn't fill up.
• Send links from other apps straight to the space they belong in.

MADE FOR IPHONE AND IPAD
• Search, open tabs and run commands from one bar.
• Peek at a link without leaving the page, read articles in Reader, and keep a video playing picture-in-picture.
• On iPad: the sidebar beside the page, several windows, split view and Stage Manager.
• Widgets, Spotlight, Shortcuts, a Focus filter and a Share extension.

SYNC THAT ONLY YOU CAN READ
• Spaces, tabs, history, passwords and open tabs sync with Compass on your other devices.
• Everything is encrypted on your device before it leaves it. The sync server stores it encrypted and can't read it.
• No account: pair a device with a code or a QR code shown by Compass on another device, or with your recovery code.
• Send a tab to your other devices.

CLAUDE, WITH YOUR OWN KEY
• Ask Claude about the page you're reading, have it find, click and fill in things, or work across your tabs.
• Claude names your folders, tidies Today and shortens long tab titles, if you turn that on.
• Claude uses your own Claude API key or endpoint. Your conversations go straight to it, not through us.

THE REST OF A BROWSER
• Extensions for the web: install Chrome and Firefox extensions such as 1Password, uBlock Origin Lite and Dark Reader.
• Your saved passwords fill in other apps too, with Face ID.
• Copy Link leaves out tracking parameters.
• Private tabs, downloads in the background, history, find in page and per-site settings.

PRIVATE BY DEFAULT
No ads, no analytics, no tracking, and no account. What you browse stays on your device unless you turn on sync.
```

## Keywords (100 characters at most, comma-separated)

```
browser,tabs,spaces,sidebar,arc,sync,private,claude,ai,extensions,web,folders,ipad,safari,chrome
```

## URLs

- **Support URL:** https://compassbrowser.org
- **Marketing URL:** https://compassbrowser.org
- **Privacy policy URL:** https://compassbrowser.org/privacy

## Categories

- **Primary:** Utilities (where Chrome, Firefox and Edge are).
- **Secondary:** Productivity.

## Age rating

The questionnaire's answers: every content item **None**, and
**Unrestricted Web Access: Yes** (it's a browser). Apple rates that 18+
(17+ before 2026's age ratings), as for every browser.

## App Privacy

From the privacy policy (website/privacy.html) and the Play data safety
draft (android/play/data-safety.md), which follow the same rules: data
end-to-end encrypted on the device isn't collected, pages the user browses
are the open web, and Claude goes to the user's own key.

- **Data collection:** Yes, only **Identifiers → Device ID**: the sync
  chain's identifier, the device list entry (device name as iOS reports it,
  model, app version, last sync) and the push token, kept by the sync server
  while the device syncs, and only with sync on.
  - Use: **App Functionality**.
  - Linked to the user's identity: **No** (no accounts).
  - Used for tracking: **No**.
- **Tracking:** No.

## App Review information

- **Sign-in required:** No.
- **Notes:**

```
Compass Browser is a web browser. No account is needed and nothing is required to try it: open the app and browse.

- Sync is optional and has no accounts: devices pair with a code shown by Compass on another device. Data is end-to-end encrypted before it leaves the device.
- Claude features are optional and use the user's own Anthropic API key (Settings > Claude); nothing is sent unless the user adds a key.
- Web extensions (Settings > Extensions) run only in web content through WebKit's WKWebExtension API, as in other browsers on the App Store; they don't change the app's own code (guideline 2.5.2).
- Copy Link removes tracking parameters using Firefox's public list, bundled in the app.
```

## Export compliance

`ITSAppUsesNonExemptEncryption` is `false` in Info.plist (only the
system's HTTPS and CryptoKit), so App Store Connect doesn't ask.

## Screenshots

- iPhone 6.9" (1320 × 2868): iPhone 18 Pro Max simulator.
- iPad 13" (2064 × 2752): iPad Pro 13-inch (M5) simulator.

# Italian (primary language in App Store Connect)

App Store Connect's primary language for the app is Italian, so this is the
listing most of the world falls back to; English (U.S.) above is a second
localization.

## Sottotitolo

```
Spazi, sync privata e Claude
```

## Testo promozionale

```
Gli spazi separano lavoro, casa e progetti. Sincronizza con Mac, PC e Android, cifrato end-to-end e senza account. Chiedi a Claude di qualsiasi pagina.
```

## Descrizione

```
Compass è un browser che tiene in ordine le tue schede. È il Compass che forse usi già sul Mac, sul PC Windows o su Android, ora su iPhone e iPad.

SPAZI
• Tieni separati lavoro, casa e progetti: ogni spazio ha le sue schede fissate, cartelle, colori e tema.
• Fissa le schede a cui torni spesso. I preferiti restano in cima a ogni spazio.
• Oggi raccoglie le schede che apri strada facendo e dopo un po' le sposta nell'archivio, così la barra laterale non si riempie.
• Manda i link dalle altre app direttamente nello spazio a cui appartengono.

PENSATO PER IPHONE E IPAD
• Cerca, apri schede ed esegui comandi da un'unica barra.
• Dai un'occhiata a un link senza lasciare la pagina, leggi gli articoli in Lettura e tieni un video in picture-in-picture.
• Su iPad: la barra laterale accanto alla pagina, più finestre, Split View e Stage Manager.
• Widget, Spotlight, Comandi, un filtro Full immersione e un'estensione di condivisione.

SINCRONIZZAZIONE CHE SOLO TU PUOI LEGGERE
• Spazi, schede, cronologia, password e schede aperte si sincronizzano con Compass sugli altri tuoi dispositivi.
• Tutto viene cifrato sul dispositivo prima di uscirne. Il server di sincronizzazione lo conserva cifrato e non può leggerlo.
• Nessun account: associa un dispositivo con un codice o un codice QR mostrato da Compass su un altro dispositivo, oppure con il tuo codice di recupero.
• Invia una scheda agli altri tuoi dispositivi.

CLAUDE, CON LA TUA CHIAVE
• Chiedi a Claude della pagina che stai leggendo, fagli trovare, cliccare e compilare, o lavorare tra le tue schede.
• Claude dà un nome alle cartelle, riordina Oggi e accorcia i titoli lunghi delle schede, se lo attivi.
• Claude usa la tua chiave o il tuo endpoint dell'API di Claude. Le conversazioni vanno direttamente lì, non passano da noi.

TUTTO IL RESTO DI UN BROWSER
• Estensioni per il web: installa estensioni di Chrome e Firefox come 1Password, uBlock Origin Lite e Dark Reader.
• Le password salvate si compilano anche nelle altre app, con Face ID.
• Copia link toglie i parametri di tracciamento.
• Schede private, download in background, cronologia, trova nella pagina e impostazioni per sito.

PRIVATO PER IMPOSTAZIONE PREDEFINITA
Niente pubblicità, niente analisi, niente tracciamento e nessun account. Ciò che navighi resta sul tuo dispositivo, a meno che tu non attivi la sincronizzazione.
```

## Parole chiave

```
browser,schede,spazi,barra laterale,arc,sync,privacy,claude,ai,estensioni,web,cartelle,ipad,safari
```
