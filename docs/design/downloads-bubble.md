# Downloads bubble

Status: desktop (Mac, Windows, Linux), 26 November 2026. Based on Chromium
157.0.8103.51.

## Goal

Arc's Library showed downloads and screenshots in one place. Compass keeps
upstream's downloads bubble and gives it two tabs instead of a new page:

- **Downloads**: everything in the download folder, not only what the
  browser downloaded (AirDrop, Mail, other apps).
- **Screenshots**: the system's screenshots and screen recordings, from the
  folder the system saves them to. On macOS that's Screenshot.app's choice
  (Options → Save to).

Not goals: a Library page, easels or boosts in the bubble (easels stay in
Pinned), or files from other folders.

## The bubble

Upstream's full downloads bubble (`DownloadDialogView`) is 420 pixels wide
instead of 320: its rows are 396 (`compass::downloads_bubble::DialogWidth()`),
in cards 12 pixels in. Each list scrolls past 520 pixels instead of 450. The
partial bubble that appears when a download finishes is unchanged.

The header's title gives way to two tabs, **Downloads** and **Screenshots**
(`TabStrip`), next to upstream's close button. The bubble always opens on
Downloads.

### Look

The settings window's (settings-window.md, "Visual design"), in views
(`compass::BubbleLook`, `ComputeBubbleLook()`, `browser/ui/views/
bubble_look.h`, which the share bubble uses too: sync.md, "UI (desktop)"):

- The bubble's background, its frame too, is the dialog's background with
  the window's space's tint over it (`ToTintColor(…, kPane)`, 14 %, 16 % in
  dark mode). None for a grey space or off the record.
- The tabs are a segmented control: a track (the text color at 7 %), the
  selected tab a white pill with a soft shadow, its icon (a download arrow,
  a screenshot region) in the space's color, the system's blue for a grey
  space.
- The lists are captioned groups: "Recent downloads" (upstream's rows) and
  "In your Downloads folder", each a 12-pixel rounded card of translucent
  white with hairlines from where the names start.
- Screenshots: "In ~/Downloads" and a pill button (Show in Finder, Open
  Folder elsewhere) in the space's color, then the tiles, with a 10-pixel
  radius and a thin frame so white pages don't melt into the background.
- Scrolling contents paint on an opaque layer (labels require it), so the
  scrollers' background is the tinted color itself, not transparent.

### Downloads

1. Upstream's rows for the browser's recent downloads, as they are
   (progress, warnings, Retry, the security page).
2. "Also in your Downloads folder" (only when there are rows above it), then
   the folder's other files, newest first, at most 100. The files upstream's
   rows show are left out. So are screenshots, when the screenshot folder is
   the download folder (macOS's default is the Desktop, but people often
   pick Downloads).
3. Upstream's "Show all downloads".

A file row (`FolderFileRow`, a `HoverButton`) shows the file's preview or
icon, its name (shortened in the middle), and its size and when it arrived
("5 mins ago", then the date). Clicking it opens the file and closes the
bubble. Dragging it drags the file out. Its menu has Open, Show in Finder
(Show in Folder), Copy (pictures: the image, to the clipboard) and Move to
Trash (Move to Recycle Bin).

The folder's entries skip hidden files, folder metadata (`.DS_Store`,
`desktop.ini`, `.localized`) and partial downloads (`.crdownload`, Safari's
`.download`, Firefox's `.part`). Folders are listed and open in the file
manager.

"Arrived" is macOS's date added (`NSURLAddedToDirectoryDateKey`), Windows'
creation time (a copy or a download makes the file anew), and on Linux the
last change.

### Screenshots

A grid of three columns of 16:10 tiles, newest first, at most 60. Videos
get a play badge. A tile does what a row does (click, drag, menu); its
tooltip says its name and when. "Open Screenshots Folder" closes the tab.
With none: "No screenshots yet. They're saved to ~/Desktop."

What counts as a screenshot:

- **macOS**: the files macOS marked as captures. Screenshots and screen
  recordings carry the extended attribute
  `com.apple.metadata:kMDItemIsScreenCapture` (Spotlight's
  `kMDItemIsScreenCapture`), with `kMDItemScreenCaptureType` and the
  captured rect beside it. The folder (often the Desktop) holds other
  pictures too, so only marked files show.
- **Windows and Linux**: every picture and video in the screenshot folder,
  which holds nothing else.

The folder (`downloads_folder::SystemScreenshotFolder()`), read every time
the lists refresh, so changing it in Screenshot.app takes effect at once:

- **macOS**: `com.apple.screencapture`'s `location-screenshot` (macOS 26
  keeps screenshots' and recordings' folders apart), else `location`
  (earlier versions), else the Desktop. Stored with a tilde
  (`~/Downloads`), expanded; a folder that doesn't exist is skipped.
  `CFPreferencesAppSynchronize()` first, since Screenshot.app writes them
  from another process.
- **Windows**: the Screenshots known folder (`FOLDERID_Screenshots`,
  Pictures\Screenshots unless moved), where Win+PrtScn and the Snipping
  Tool save.
- **Linux**: the XDG pictures folder's Screenshots (GNOME, KDE's
  Spectacle).

Compass's own screenshots (screenshots.md, "Save to Screenshots") go to the
same folder, and on macOS get the same attribute, so they list with the
system's.

### Following the folders

While the bubble is open, a `base::FilePathWatcher` per folder (on a
blocking sequence, `FolderWatch`) refreshes the lists 250 ms after the last
change: a download that finishes renames its partial file, and a trashed
file goes. Listing runs off the UI thread (`ListFolder()`), and both lists
are rebuilt from it. Previews are kept for the bubble's life by path, size
and date, so a refresh doesn't load them again.

### Previews

`FileThumbnails`:

- **macOS**: QuickLook's thumbnail (`QLThumbnailGenerator`, every
  representation): a picture's or a video's frame, a PDF's first page,
  documents, else the file's icon.
- **Windows and Linux**: pictures up to 30 MB, read off the UI thread and
  decoded out of process (`data_decoder::DecodeImageIsolated()`), scaled
  down to cover the tile; everything else gets its icon (`IconManager`).

Made at 2x. Pictures and videos fill their square or tile (cropped in the
middle); icons fit in it.

## With no recent downloads

The downloads button is pinned by default: `toolbar.pinned_actions` gets
`kActionShowDownloads` after upstream's defaults
(`compass::OverrideProfilePrefDefaults()`), so it stays in the sidebar
header (command-bar.md, "Moving the controls") with nothing downloaded.
Unpinning it from its menu brings back upstream's button that comes and
goes. A profile that already changed its pinned actions keeps its list.

Pressed with no recent downloads, upstream opens chrome://downloads.
Compass shows its own bubble instead (`ShowWithoutRecentDownloads()`): the
same header (tabs and close button), the library without upstream's rows,
and a "Show all downloads" footer built the same way. It's anchored to the
downloads button through `PinnedToolbarActions::GetBubbleAnchorAsync()`, as
upstream's is. If there's no button to anchor to, chrome://downloads opens
as before.

Pressing the button again closes it. Pressing the button also takes focus
from the open bubble, which closes it first; a press within 300 ms of the
bubble closing doesn't open it again.

## Code

- `browser/downloads_folder/`: what the lists hold and the platform parts
  (`folder_files{,_mac.mm,_win.cc,_linux.cc}`): `ListFolder()`,
  `DownloadsTabFiles()`, `ScreenshotsTabFiles()`, `ScreenshotFolder()`,
  `IsScreenCapture()`, `MarkAsScreenCapture()`, `MoveToTrash()` (macOS
  `NSFileManager`, Windows `SHFileOperation` with undo, Linux `gio
  trash`). No UI dependencies; unit tested.
- `browser/ui/views/downloads_bubble/`: `DownloadsLibraryView` (the tabs'
  pages, rows, tiles, menus, drag), `FileThumbnails`, and the hooks
  (`downloads_bubble.cc`).
- `downloads_bubble_hooks.h`, in its own dependency-free target
  (`//compass/browser/ui:downloads_bubble_hooks`), is what upstream calls:
  - `download_dialog_view.cc`: the rows' width is `DialogWidth()`, and the
    constructor ends with `ExtendDownloadDialog()`. That moves upstream's
    rows (a `ScrollView`, now as tall as its rows) and footer into the
    library, which scrolls both lists itself, hides the title and puts the
    tabs in its place. If upstream's layout isn't the expected one (header,
    rows, footer), it's left alone.
  - `download_toolbar_ui_controller.cc`: `InvokeUI()` asks
    `ShowWithoutRecentDownloads()` before opening chrome://downloads.
  - `chrome/browser/ui/views/download/BUILD.gn`: the hooks' dependency.

## Not yet

- A shortcut or a command bar command for Screenshots.
- Screen recordings' length on their tiles.
- Arc's Library media in the import (arc-import.md).
- Android and iOS.

## Tests

`compass_unittests`: `FolderFilesTest` (kinds, hidden entries, the two
tabs' filters, a real folder listed, macOS's mark written and read, the
screenshot folder found); `PrefDefaultsTest.DownloadsButtonPinned` and
`NoPinnedActionsWithoutTheToolbar`.

`compass_browsertests`: `DownloadsBubbleBrowserTest`: the button is pinned;
with no recent downloads it opens the tabs on both folders, follows a new
file, and closes on a second press; `ExtendDownloadDialog()` on a dialog
laid out as upstream's. `EaselCaptureBrowserTest.ScreenshotSavesTheFullPage`
saves to the screenshot folder.
