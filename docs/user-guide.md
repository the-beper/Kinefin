# Kinefin user guide

## Contents

- [What Kinefin is](#what-kinefin-is)
- [Install and update](#install-and-update)
- [Sign in](#sign-in)
  - [Add a server](#add-a-server)
  - [Test a server](#test-a-server)
  - [Sign in with a password or Quick Connect](#sign-in-with-a-password-or-quick-connect)
  - [Several users on one server](#several-users-on-one-server)
- [Browse](#browse)
  - [Home](#home)
  - [Act on a card](#act-on-a-card)
  - [Movies and Shows](#movies-and-shows)
  - [Show more library tabs](#show-more-library-tabs)
  - [Item details](#item-details)
  - [Search](#search)
  - [Favorites](#favorites)
  - [Playlists](#playlists)
  - [Queue](#queue)
- [Play](#play)
  - [Control playback](#control-playback)
  - [Audio and subtitle tracks](#audio-and-subtitle-tracks)
  - [Keep watching while you browse](#keep-watching-while-you-browse)
  - [Play the next item automatically](#play-the-next-item-automatically)
  - [Jump between chapters](#jump-between-chapters)
  - [Choose a quality](#choose-a-quality)
  - [Save a clip or a GIF](#save-a-clip-or-a-gif)
  - [Watch together with SyncPlay](#watch-together-with-syncplay)
  - [Control other sessions](#control-other-sessions)
- [Download for offline use](#download-for-offline-use)
  - [Automatic downloads for a series](#automatic-downloads-for-a-series)
  - [Export to a folder](#export-to-a-folder)
  - [Work offline](#work-offline)
- [Cast to another device](#cast-to-another-device)
- [Search from KRunner](#search-from-krunner)
- [Use the keyboard or a gamepad](#use-the-keyboard-or-a-gamepad)
  - [Keyboard](#keyboard)
  - [Gamepad (experimental)](#gamepad-experimental)
- [Troubleshooting](#troubleshooting)
  - [The server is not reachable](#the-server-is-not-reachable)
  - [Kinefin asks for the password again](#kinefin-asks-for-the-password-again)
  - [Playback stutters](#playback-stutters)
  - [A video transcodes when it should direct-play](#a-video-transcodes-when-it-should-direct-play)
  - [There is no sound over HDMI or S/PDIF passthrough](#there-is-no-sound-over-hdmi-or-spdif-passthrough)
  - [The Download actions are missing](#the-download-actions-are-missing)
  - [The Admin Dashboard entry is missing](#the-admin-dashboard-entry-is-missing)
  - [Search Online Subtitles… is missing](#search-online-subtitles-is-missing)
  - [A cast device is not listed](#a-cast-device-is-not-listed)
  - [KRunner shows no Kinefin results](#krunner-shows-no-kinefin-results)
  - [The picture-in-picture window falls behind other windows](#the-picture-in-picture-window-falls-behind-other-windows)
- [Where Kinefin keeps its files](#where-kinefin-keeps-its-files)

## What Kinefin is

Kinefin is a native KDE Plasma 6 client for Jellyfin media servers. The interface is built with Kirigami and the video is played by libmpv. It does not embed the Jellyfin web client.

It needs:

- A Jellyfin server you can sign in to. Kinefin is tested with Jellyfin 12.4.
- Plasma 6 on Wayland.
- Flatpak, the only way Kinefin is packaged.

Kinefin covers movies and TV series. Music and audiobooks are not built yet: those libraries show on Home but stay disabled with "Not supported yet".

Server administration is not part of Kinefin. If you are an administrator, **Admin Dashboard** in the main menu opens the server's web interface in your browser. User management, library management, plugins and Live TV setup happen there.

The [settings reference](settings.md) lists every setting with its default. Where a task needs one, this guide names it with its place in the app, such as **Settings**, **Video**.

## Install and update

Kinefin has no Flathub listing yet. It is installed from a Flatpak bundle, `Kinefin-x86_64.flatpak`.

To install the bundle for your user:

1. Open a terminal in the folder that holds the bundle.
2. Run `flatpak install --user --bundle Kinefin-x86_64.flatpak`.
3. Start Kinefin from the application launcher, or run `flatpak run io.github.the_beper.Kinefin`.

To update, run the same command with `--reinstall` added: `flatpak install --user --reinstall --bundle Kinefin-x86_64.flatpak`. Your servers, settings and downloads under `~/.var/app/io.github.the_beper.Kinefin/` are kept.

Kinefin checks for a new release a few seconds after it starts. When one exists, a dialog asks whether to open the release page in your browser. Nothing is installed automatically. To turn the check off, switch off **Check For Updates** in **Settings**, **General**. While it is on, **Check for Updates** in **About Kinefin** checks at once.

A second launch does not open a second window. It raises the running one.

## Sign in

### Add a server

To add a server:

1. On the **Servers** page, enter the server's host name or IP address in **Address**.
2. Enter a **Port** only if the server does not use a standard one. Leave it empty for **Automatic**.
3. Press **Connect**.

Write the address without `https://` or `http://`, a path or a user name. The page shows a warning while the host or the port (1 to 65535) is not valid. A host with one colon is read as `host:port`, and an address with several colons as IPv6.

Without a scheme, Kinefin tries HTTPS first and falls back to HTTP:

| You enter | Order tried |
| --- | --- |
| `server.example` | `https` on 443, `https` on 8920, `http` on 8096, `http` on 80 |
| `server.example` and port `9000` | `https` on 9000, then `http` on 9000 |

A saved server keeps the exact address that worked and is never retried on another scheme.

If the server presents an invalid TLS certificate, Kinefin stops with a message and does not fall back to HTTP. Fix the certificate, the address or the port. For a server with a self-signed certificate you can switch on **Ignore Certificate Errors** in **Settings**, **General**.

> [!WARNING]
> Over plain HTTP your password and everything you watch travel unencrypted. Before Kinefin sends credentials to an HTTP address that is not on this computer, it asks you to confirm with **Continue Anyway**. After an automatic fallback the warning says that HTTPS did not answer and names the address that did.

While Kinefin connects, the **Servers** page shows **Cancel**.

### Test a server

**Test** on a saved server, or on the address you typed, checks whether it answers. It tries the same addresses as Connect, waits 4 seconds per attempt, and shows a notification naming the address that answered. Nothing is tested when the page opens.

### Sign in with a password or Quick Connect

After the server answers, the sign-in page offers a password or Quick Connect. Users the server lists as public appear as tiles; selecting one fills in the name.

To use Quick Connect, choose it on the sign-in page and confirm the code it shows from a Jellyfin client that is already signed in to the same server.

Kinefin never stores your password. The access token is kept in KWallet.

### Several users on one server

Each user you sign in as is saved with the server. What happens at the next start depends on how many users are saved:

| Saved users | Result |
| --- | --- |
| One, remembered | Signs in without a prompt |
| One, set to **Ask for Password** | Password prompt |
| Two or more | The user picker, a grid of avatars, with **Add User** as the last tile |

In the picker, arrows move, Return or Space chooses, and a lock badge marks a user without a stored token. The Menu key, a right click or the overflow button opens **Ask for Password** and **Remove User…**. **Switch Server**, at the top of the page, goes back to the **Servers** page.

**Ask for Password** stores no token for that user, so Kinefin asks every time. It is off by default.

The main menu, opened from the avatar at the right end of the header, has the account actions:

| Action | Effect |
| --- | --- |
| **Switch User** | Opens the picker and keeps the current user's token and queue |
| **Sign Out** | After confirmation, deletes the user's session and queue on this computer; the server session is not ended |
| **Switch Server** | Returns to the **Servers** page and keeps the session |

**Remove User…** and removing a server delete the saved session the same way. Downloads stay on disk and come back when that user signs in to that server again.

## Browse

The header holds the tabs **Home**, **Downloads**, **Favorites** and **Playlists**, then **Movies** and **Shows** when the server has a library of that type. Further right are the search box, the **Queue** button with its count, and the main menu. When the window is narrow the labels become icons with tooltips, and the search box becomes a button.

Escape and Alt+Left go back. The mouse back button does too. F5 reloads the page in front.

### Home

Home shows these rows:

| Row | Content |
| --- | --- |
| Continue Watching | Items with a saved position. **Show All** opens the whole list |
| Next Up | The next episode of series you follow |
| Favorites | Up to 24 of your favorite movies and series in random order, a new pick at each load. **Show All** opens the Favorites tab |
| Recently Added | One row per movie or series library. **Show All** opens that library, newest first |

Below the rows, under **Libraries**, is one tile per library. A tile of a movie or TV library opens its tab with that library selected, and a collections tile opens a library page.

Cards show a rating, the minutes left, a played check or the number of unplayed episodes, and a **New** chip for items added in the last 7 days that you have not played. A series card also shows its seasons, episodes and a watched-episode bar. Hovering a Continue Watching card shows a **Play** disc and a **Mark as Watched** button.

The **Favorites** row, the **Libraries** tiles, the **New** chip and the library sub-tabs can be switched off in **Settings**, **Library**.

### Act on a card

The overflow button on a card, a right click, the Menu key or Shift+F10 open the card menu:

- **Open**, **Play Now** and **Cast…** for playable items; a Continue Watching card has **Resume** and **Play from Beginning** in place of **Play Now**
- **Go to Series** on an episode
- **Play Next** and **Add to Queue**
- **Add to Favorites** or **Remove from Favorites**
- **Add to Playlist…** for a movie, episode or video
- **Mark as Watched** or **Mark as Unwatched**

The entries shown depend on the item's type. Marking as watched from a card shows a notification with **Undo**.

### Movies and Shows

**Movies** and **Shows** list every library of that type. When you have two or more such libraries, a selector lets you pick one or **All Libraries**. Kinefin remembers the selection, the view options, the filters and the sort for each library.

The toolbar has the sub-tabs, **Filters**, **Sort** and the view options. Search in these tabs filters the list in front of you instead of opening the search page.

To filter:

1. Open **Filters**.
2. Check the options you want. Options of one section are combined with OR, different sections with AND.
3. Press **Apply**. **Clear** drops the choices not applied yet.

| Section | Options | Applies to |
| --- | --- | --- |
| Status | Played or Unplayed, Resumable, Favorites | Resumable: movies only |
| Features | Subtitles, Trailer, Special Features, Theme Song, Theme Video | Movies, series and episodes |
| Video Quality | HD or SD, 4K, 3D | Movies and episodes |
| Video Type | Blu-ray, DVD | Movies and episodes |
| Series Status | Continuing, Ended, Unreleased | Series |
| Episodes | All, Specials, Missing, Unaired | Episodes |
| Genres and Tags | Any number; tags have a search field | All |
| Ratings and Years | Any number | All |

A section that does not apply to the current list is left out. **Clear Filters** removes the filters of the current view only; search, sort and library stay. If a filter leaves nothing, the page says so and offers **Clear Filters**.

Sort keys differ by list: movies have 10 (Name, Random, Community Rating, Critic Rating, Date Added, Date Played, Parental Rating, Play Count, Release Date, Runtime), series 8, episodes 7 and collections 5. **Ascending Order** and **Descending Order** set the direction. **Random** shows one page of 100 items, and **Reshuffle** asks for another.

**Grid View** and **List View** switch the layout. **View Options** has:

| Option | Values |
| --- | --- |
| Image type | Poster, Banner, Disc, Logo, Thumbnail |
| Show Title, Show Year | On or off |
| Card Layout | Off hides the text under each card |

The grid scrolls continuously. **Return to Top** sits at the right end of the toolbar. Going back from an item restores your scroll position.

### Show more library tabs

Sub-tabs sit in the toolbar of **Movies** and **Shows**. The main tab is always there. Each other one is a switch in **Settings**, **Library**.

| Sub-tab | In | Shows |
| --- | --- | --- |
| Suggestions | Movies, Shows | Server recommendations |
| Favorites | Movies, Shows | Favorite movies or series of the chosen library |
| Collections | Movies, Shows | Collections of all libraries |
| Genres | Movies, Shows | One rail per genre; **Show All** opens that genre in the grid |
| Upcoming | Shows | Episodes yet to air |
| Networks | Shows | Series grouped by network |
| Episodes | Shows | All episodes of the chosen library |

Suggestions in **Shows** and Collections in both tabs ignore the library selector and cover all libraries. Which sub-tabs are on by default is in the [settings reference](settings.md#library-library).

### Item details

Selecting a card opens the details page: a hero with the backdrop, logo, a facts line (year, rating, runtime, end time) and **Play** or **Resume**. Under it come media badges (resolution, HDR, codecs, subtitle count), the overview, the credits, the cast and the related rows. The page toolbar has **Play Next**, **Add to Queue**, the favorite toggle, **Mark as Watched** or **Mark as Unwatched**, and **Media Info**. **Media Info** opens a dialog with the container, size, bit rate and streams of each source, and **Copy** puts the text on the clipboard.

Before you play, choose the audio track and the subtitles (or **Off**) with **Audio** and **Subtitles** in the row under the facts, which appears only when there is a choice; **Play** and **Resume** use them. When the item has a resume point, **Play from Beginning** starts it from the first second. **Trailer** opens the item's first trailer in your browser. **Copy Stream URL** puts the URL of the first source on the clipboard, without a token. **Refresh Metadata** asks the server to fetch the item's metadata and images again.

**Add to Collection…** on a movie or a show puts it into an existing collection or a new one. An administrator can change the item on the server. **Delete Media…** removes a movie, episode or video when the server allows it, after a confirmation; the server deletes the file and it cannot be recovered. **Edit Metadata…** changes the name, overview, year, official rating and genres. **Identify…** on a movie or a show searches the metadata providers and applies the match you pick. **Edit Images…** replaces or removes the primary image, the backdrop or the logo. **Edit Subtitles…** uploads an external subtitle file or deletes an external track. Clicking a cast member opens the movies, shows and episodes that person appears in.

A series page lists seasons and one Next Up entry, a season page lists its episodes, and an episode page links to its series and season. A collection page lists its first 100 items. **Add to Playlist…** is on movie, episode and video pages.

### Search

Ctrl+F, or typing in the header box, opens the search page. It shows four groups of up to 24 results: Movies, Series, Episodes and Collections. Selecting a result opens its details page. Search covers titles only, not people.

### Favorites

The **Favorites** tab lists everything you marked as a favorite, across all movie, TV and collection libraries, up to 1000 items. A banner says when the list may be incomplete, with **Retry** after a failed library. You group the list by Type, Library, Genre, Year or None. This page is read-only: set favorites from a card menu or a details page.

### Playlists

The **Playlists** tab lists your video playlists. A playlist page shows its entries in order, with **Play** and **Shuffle** at the top. **Play** and **Shuffle** replace the queue with the playlist, and the Queue page's **Undo** takes that back.

To add an item to a playlist, choose **Add to Playlist…** in a card menu or on a details page, then pick a playlist or **New Playlist…**. A video that is already in the playlist is not added twice.

To edit a playlist:

- Ctrl+Up and Ctrl+Down, or **Move Up** and **Move Down** in the row menu, reorder entries.
- Delete, or **Remove from Playlist**, removes one. There is no confirmation and no undo.
- **Rename…** (also F2) and **Delete…** are in the page toolbar overflow and in a playlist card's menu. Deleting asks first.
- **New Playlist…** in the toolbar creates an empty playlist.

Changes show at once and are sent to the server in order. If the server refuses one, for example on a playlist shared with you, the order is put back and a message appears.

### Queue

The queue is an ordered list of up to 500 movies, episodes and videos, each at most once. Kinefin keeps one queue per profile, server and user and restores it at sign-in.

To add to it, use **Play Next** (top of the queue) or **Add to Queue** (end) on a card or a details page. On a season, series or collection they add the playable episodes or children: a season from its first unplayed episode, a series from its Next Up episode.

The **Queue** button in the header opens the page. Its toolbar has **Resume Queue**, **Play From Here**, **Undo** and **Clear Queue** (with confirmation). In the list:

| Key | Action |
| --- | --- |
| Return | Open the item |
| Ctrl+Up, Ctrl+Down | Move the item |
| Delete | Remove the item |
| Ctrl+Z | Undo the last change |

The row menu offers **Move Up**, **Move Down**, **Remove** and **Go to Item**. Dragging rows is not supported.

An episode played on its own is not added to the queue. The player still lets you step through its series with **Next** and **Previous**.

## Play

To play, press **Play** or **Resume** on a card or details page, or press Return on a focused card. Kinefin direct-plays the file when the server allows it and the file passes the device rules. Otherwise the server transcodes. **Media Info** and the player's **Playback Info** panel show which one is happening.

The video fills the window. Bars with the controls appear when you move the pointer and hide 5 seconds later. They stay while playback is paused, while the pointer rests on them and while a menu is open.

### Control playback

The top bar has **Back** (leaves the player, playback continues in the mini player), the title and episode line, and **Cast** and **Picture-in-Picture** buttons. In a SyncPlay group a group button also shows there.

The bottom bar has the timeline with a thumbnail preview while you drag, **Previous** and **Next** (when there is one), **Pause** or **Resume**, a speaker button with a volume slider, **Audio**, **Subtitles**, **Full Screen Mode** and **More Actions**. The time at the left of the timeline is the current position. The one at the right toggles total and remaining time. **Ends at** shows the clock time the item ends at the current speed. **Skip Intro**, **Skip Recap** or **Skip Outro** appears during a segment the server knows.

A right click on the video, the Menu key or Shift+F10 open a context menu with the main actions. A single click on the video pauses or resumes, and a double click toggles full screen. The mouse wheel anywhere over the player changes the volume by 5.

| Key | Action |
| --- | --- |
| Space | Play or pause |
| M | Mute or unmute |
| +, - | Volume up, down |
| F, F11 | Toggle full screen |
| Left, Right | Skip back, forward 10 seconds |
| Down, Up | Skip back, forward 1 minute |
| PgDown, PgUp | Previous, next chapter |
| I, O | Set the clip start, end |
| Escape | Close the open popup, cancel Up Next, leave full screen, then leave the player |
| Return | Play the Up Next item now |

**More Actions** holds the rest:

| Entry | Use |
| --- | --- |
| **Show Mini Player** | Docks the video and leaves the page |
| **Shuffle**, **Repeat** | Off, All or Current Item; Shuffle needs a queue of two or more |
| **Playback Speed** | 0.5x to 4x |
| **Aspect Ratio** | Auto, Zoom, Stretch, 4:3, 16:9, Widescreen for 4:3 Video, Original Size; applies to this item only |
| **AV Sync** | Audio delay |
| **Quality** | See [Choose a quality](#choose-a-quality) |
| **Chapters** | Jump to a chapter |
| **Clip** | See [Save a clip or a GIF](#save-a-clip-or-a-gif) |
| **Sleep Timer** | Stops playback after 15 to 120 minutes, at the **End of Current Item** or at the **End of Queue** |
| **Now Playing Queue** | A panel with the queue |
| **Screenshot** | Saves the current frame |
| **Keep Controls Visible** | Keeps the bars up |
| **Forget Series Choices** | Clears what Kinefin remembered for this series |

The Audio menu ends with **Night Mode**, which raises quiet dialogue and lowers loud scenes. It is the same switch as in **Settings**, **Audio**.

**Playback Info**, also in **More Actions**, opens a panel over the video with the play method, stream sizes, hardware decoding, dropped frames and the reasons for a transcode.

### Audio and subtitle tracks

The **Audio** and **Subtitles** menus list the tracks of the file. **Subtitles** also lists the external subtitle files the server has for the item, and **Off**.

To show a second subtitle line, for example a translation, choose a text track in **Secondary Subtitles** at the end of the Subtitles menu. The second line sits at the opposite edge of the screen from the first. One delay setting moves both.

An external subtitle on a transcode is extracted by the server on first use. That can take minutes for a large file, and a notification says so.

To search for a subtitle online:

1. Open **Subtitles** and choose **Search Online Subtitles…**.
2. The dialog searches in English as it opens. Pick French, German, Italian or Spanish to search in that language, or press **Search** to repeat the search.
3. Press **Download** on a result.

The entry shows only when your server user is allowed to manage subtitles, the item comes from the server and you are not in a SyncPlay group. Searching needs the OpenSubtitles plugin on the server, because Kinefin never contacts OpenSubtitles itself; without it the dialog says that no provider is installed. After the download the video reopens at the current position with the new track available. If the server cannot save the file, a message says so and the server's administrator can check where the library stores subtitles.

Kinefin remembers an explicit track choice per item. For an episode played directly it also remembers it per series and applies it to the following episodes, matching by language and forced flag. **Remember the audio track** and **Remember the subtitle track** in **Settings**, **User** turn this off.

### Keep watching while you browse

The **Back** button, Escape, Alt+Left and the mouse back button leave the player without stopping. The video moves to a mini player docked in the bottom right corner of the window while you browse. Hovering it shows **Pause** or **Resume**, **Expand Player**, **Close Mini Player**, the title and a progress line. **Close Mini Player** stops playback. Ctrl+Shift+P expands it.

**Picture-in-Picture**, in the top bar and the context menu, moves the video into a small frameless window of its own. The main window stays free to use or minimise. Controls show on hover: an X in the corner, **Pause** or **Resume**, a timeline and the time. A click pauses or resumes, and Space pauses. The X, a double click, closing the window or Ctrl+Shift+P return to the full player without stopping.

On KWin the window stays above the others. On other compositors it is an ordinary window, which the compositor may let other windows cover.

### Play the next item automatically

Near the end of an item that is followed by another, a card shows the next item's picture and title and "Starts in N seconds". That happens for items in the queue and, while **Play the next episode automatically** in **Settings**, **User** is on, for the episodes of a series.

- If the item has an Outro segment near its end, the card shows at the Outro's start and the next item starts 10 seconds later.
- Otherwise the card shows 30 seconds before the end.
- There is no card under 10 minutes of runtime, with **Repeat** set to **Current Item**, or for a lone item.

Return plays the next item now. Escape cancels, so the item ends without moving on. In the mini player the card is not shown, and the next item still starts.

To skip intros without pressing **Skip**, switch on **Skip Intros Automatically** in **Settings**, **Video**.

### Jump between chapters

Chapters come from the server. Each one but the first is a mark on the timeline, and the drag preview shows the chapter name. The **Chapters** submenu of **More Actions** lists them with their time. PgUp and PgDown step between them. PgDown goes to the start of the current chapter when you are more than 3 seconds into it. Chapters have no images.

### Choose a quality

Under **More Actions**, **Quality** pins a maximum bit rate for the current playback: **Automatic**, or a step below the file's own bit rate. The server then transcodes at that cap. The pick holds across **Next** and queue jumps until playback stops. If the stream does not open, the player shows "The selected quality could not be opened" with **Try Again** at the previous cap.

**Quality** is disabled for local files and in a SyncPlay group, and when your server user is not allowed to transcode video.

When a server stream keeps stalling, Kinefin lowers the quality by itself and shows a notification with the new bit rate. **Lower Quality on Slow Connections** in **Settings**, **Video** turns this off; the [settings reference](settings.md#video-video) has its rules.

### Save a clip or a GIF

You can save part of what plays as a video clip or a GIF.

1. Play the item to the start of the part you want and press I.
2. Play to the end and press O.
3. Open **More Actions**, then **Clip**. The submenu shows the range, or why it cannot be saved.
4. Choose the clip or the GIF.
5. Pick a file in the save dialog.

A clip lasts 1 to 60 seconds and a GIF 1 to 15 seconds. The clip is MP4 (H.264 and AAC), or WebM where H.264 encoding is not available. A progress card has **Cancel**. The clip uses the audio and subtitle tracks that play, with your subtitle style. HDR is refused, and so is a range of a stream that is no longer buffered ("The start of the clip is no longer buffered").

### Watch together with SyncPlay

SyncPlay keeps playback in step with other people on the same server.

1. Open the main menu and choose **SyncPlay**.
2. Choose **Join** on a group, or enter a name and press **Create Group**.

The entry shows only while you are signed in and the server permits SyncPlay. **Create Group** is hidden if your user may only join groups.

In a group the player waits for the others, and notifications name members who join and leave. Pause, resume, seek, **Next** and **Previous**, shuffle and repeat act on the whole group. Speed is not available and automatic intro skipping is off. The group's queue can be reordered and trimmed in the dialog. Joining a group stops anything you were playing alone.

Closing the player stops following the group but keeps you in it. **Follow Again** in the dialog catches up. A lost connection joins the group again.

### Control other sessions

The main menu's **Remote Sessions** opens a dialog listing the other sessions on the server. It refreshes every 5 seconds while open. Each session has **Pause**, **Resume** and **Stop**.

Other Jellyfin clients can also control Kinefin while you are signed in. They can send navigation keys, volume, mute, track and quality choices, and **Play**. A remote **Play** of one video plays it. Several videos become the saved queue. Remote navigation keys are ignored while a text field or a dialog has focus. Remote text messages are not shown.

MPRIS is supported: media keys and the Plasma media controls see the current item, and `Next` and `Previous` follow the queue or the series.

While playback runs, Kinefin keeps the screen from blanking. Opening and buffering count as playing.

## Download for offline use

The **Downloads** tab holds a download queue, saved files and offline playback. It needs a server user who is allowed to download media. Without that permission the Download actions are hidden and the tab shows a note.

To download an item, choose **Download** on its details page or in a card menu. Kinefin fetches the original file with no re-encoding and resumes an interrupted transfer. External text subtitles (up to 8) and chapters are saved with it, once the file completes. Embedded subtitles stay in the file.

The Downloads page has an active section and a completed one. Each row has one main button (**Play**, **Resume**, **Retry** or **Pause**) and **More Actions**: **Export…**, **Cancel Download** and **Delete Download…**, which asks first. A click on a completed row plays it. The footer shows the storage used and the number of changes waiting to sync.

**Settings**, **Downloads** sets how many downloads run at once and whether automatic downloads wait for an unmetered network.

### Automatic downloads for a series

Each series can have one rule. To set it, open the series or one of its seasons and choose **Automatic Downloads…** in the page's actions (**Edit Automatic Downloads…** once the series has a rule). A rule adds unwatched, already aired episodes only:

| Rule | Behaviour |
| --- | --- |
| **Next episodes**, with **Episodes:** 1 to 3 | Downloads the next unwatched episodes as playback advances or the queue empties |
| **Whole season**, offered from a season page | Downloads every episode of that season |
| **New episodes** | Downloads new episodes of an airing series |

New episodes are looked for at sign-in, every 6 hours and when the server reports a library change. **Stop Automatic Downloads**, on the series page or in the **Downloads** tab, removes the rule.

### Export to a folder

To copy a download out of Kinefin's own storage:

1. Choose **Export…** on a completed download.
2. Pick a folder in the dialog. Under Flatpak it is the desktop's portal dialog.

Files are named like the library: `Movies/Title (Year).ext`, and `Series/Season XX/Series - SxxEyy - Episode.ext`. You can add Kodi sidecar files (`.nfo` and `poster.jpg`) in the Downloads settings. Subtitle files are not exported. The copy inside Kinefin is kept. Exporting automatically when a download completes is shown disabled, because it needs a permanent folder permission that is not available.

### Work offline

When you have no connection, **Work Offline** on the **Servers** page opens Kinefin with the downloads of a saved user. That user must be remembered and not set to **Ask for Password**, and have at least one completed download. **Leave Offline** in the menu goes back.

Offline playback has no server session and sends no reports. Kinefin stores the position and marks an item as watched at the end or past 90 percent. The watched state is sent to the server the next time you sign in or reconnect.

## Cast to another device

Kinefin plays on DLNA (UPnP) renderers such as smart TVs and network players. Google Cast is not supported.

To cast:

1. Choose **Cast…** in a card menu, on a details page, or in the player's top bar.
2. Pick a device in the dialog. The list is built while the dialog is open. **This Computer** is the first row.
3. Wait for the device to start.

A cast from a card or details page starts at the resume point. From the player the playing video moves to the device at its position, with its audio and subtitle choice. **This Computer** brings the video back at the device's position.

The controls of the player, the keyboard, MPRIS and other sessions act on the device: pause, resume, seek, chapters, **Next**, **Previous**, volume and mute where the device supports them. Speed, aspect, track menus, subtitle search, screenshots, clips, picture-in-picture, automatic quality lowering and the Up Next card are off while casting. A chosen subtitle is burned in, which makes the server transcode. If the device refuses the file, Kinefin asks the server once more for a transcode.

**Cast…** is disabled while you follow a SyncPlay group and hidden for a downloaded file. The screen is allowed to blank while a cast plays, and the computer is kept from suspending.

When the device stops by itself, a notification appears and the player closes. If it stops answering, the player shows the error with **Continue Here**, which plays on this computer from where it got to.

> [!NOTE]
> The media address sent to the device carries no token. Anyone on your network who has it can open the stream.

## Search from KRunner

KRunner and the Plasma search fields built on it can find movies, series, episodes and collections on the signed-in server while Kinefin is running.

To turn it on, enable the Kinefin plugin in **System Settings**, **Search**, **Plasma Search**. It is off by default.

A query of 3 characters or more is sent to the server after a 300 ms pause. Return opens the item's details page, and **Play** or **Resume** sit on the row. Nothing is sent while Kinefin is closed or signed out, or when the server is on plain HTTP and not on this computer.

## Use the keyboard or a gamepad

Ctrl+Shift+/ opens the shortcuts dialog, which lists the keys below and, while a gamepad is connected, its buttons.

### Keyboard

| Key | Action |
| --- | --- |
| Ctrl+Q | Quit |
| Escape, Alt+Left | Go back |
| Ctrl+F | Search |
| F5 | Refresh |
| Ctrl+1 to Ctrl+6 | Tab by position |
| Ctrl+PgUp, Ctrl+PgDown, Ctrl+[, Ctrl+], Ctrl+Shift+Tab, Ctrl+Tab | Previous or next tab, wrapping |
| Menu, Shift+F10 | Open the item menu |
| Ctrl+Z | Undo the last change on the Queue page |
| Ctrl+Up, Ctrl+Down, Delete | Move or remove a queue or playlist item |
| F2 | Rename a playlist |
| Ctrl+S | Save user preferences |
| Ctrl+, | Settings |
| Ctrl+Shift+P | Expand the player |
| Ctrl+Shift+/ | Show keyboard shortcuts |

The player keys are in [Control playback](#control-playback). Quit has no menu entry: use Ctrl+Q or close the window.

**Enable Input Repeat** in **Settings**, **General**, controls whether a held key repeats.

### Gamepad (experimental)

Kinefin reads SDL3 gamepads and shows no notice when one connects or leaves. The D-pad moves the focus by position on screen; the left stick scrolls without moving it, and in the full player it seeks like the D-pad. Button names follow the pad: the dialog shows A, B, X, Y or Cross, Circle, Square, Triangle.

| Button | Browsing | In the player |
| --- | --- | --- |
| D-pad | Move the focus | Skip 10 seconds or 1 minute |
| Left stick | Scroll | Skip 10 seconds or 1 minute |
| South (A, Cross) | Open or activate | Play or pause, or start Up Next |
| East (B, Circle) | Go back | Cancel Up Next, leave full screen, or go back |
| West (X, Square) | Open the item menu | Open the player menu |
| North (Y, Triangle) | Play or pause | Play or pause |
| Start | Open the main menu | Toggle full screen |
| Select | Expand the player | Expand the player |
| Shoulder buttons | Previous or next tab | Previous or next item |
| Triggers | Previous or next chapter | Previous or next chapter |
| Right stick | Volume up or down | Volume up or down |

The pad only works while a Kinefin window has focus. To turn gamepad input off, switch off **Gamepad (Experimental)** in **Settings**, **General**, or set `sdlEnabled=false` in the `[main]` group of `kinefin.conf` while Kinefin is closed; Kinefin then never opens an input device. The mapping is fixed: buttons cannot be remapped. [Where Kinefin keeps its files](#where-kinefin-keeps-its-files) has the folder.

## Troubleshooting

### The server is not reachable

1. Press **Test** on the **Servers** page. The message names the address that answered, or says what failed.
2. Check the host and the port. Enter a port only for a non-standard one.
3. If the message mentions the certificate, fix the certificate. For a self-signed certificate you can switch on **Ignore Certificate Errors** in **Settings**, **General**.
4. If only HTTP answers, confirm the warning knowing the traffic is not encrypted.

### Kinefin asks for the password again

A user set to **Ask for Password** is asked every time. Otherwise the saved token may be gone: it is stored in KWallet, and a wallet that cannot be read, or an expired token on the server, leads back to the password prompt. Sign in again. The prompt does not need the server to be added again.

### Playback stutters

Playback stutters when the connection cannot keep up with the stream or the machine cannot decode it.

1. Pin a lower bit rate in **More Actions**, **Quality**, so the server transcodes.
2. Check that **Lower Quality on Slow Connections** is on in **Settings**, **Video**.
3. Increase **Cache Size** in **Settings**, **Video**.
4. Try another **Hardware Decoding** value in **Settings**, **Video**.
5. Press **Playback Info** in **More Actions** to see whether the item direct-plays or transcodes, and how many frames drop.

### A video transcodes when it should direct-play

The device rules force a transcode for some sources. Dolby Vision profile 5 transcodes by default. Several **Force Transcode** switches and **Always Force Transcode** in **Settings**, **Video**, force it for HEVC, AV1, 4K, HDR, 10-bit video and everything else. Check those switches. **Playback Info** shows the server's reasons.

### There is no sound over HDMI or S/PDIF passthrough

Passthrough needs the **Device Type** set to S/PDIF or HDMI and the codecs your receiver supports switched on, in **Settings**, **Audio**. **Night Mode** turns passthrough off while it is on.

### The Download actions are missing

The server user lacks permission to download media. Ask the server's administrator.

### The Admin Dashboard entry is missing

The entry shows only for server administrators.

### Search Online Subtitles… is missing

The entry has conditions, listed in [Audio and subtitle tracks](#audio-and-subtitle-tracks).

### A cast device is not listed

Devices appear only while the **Cast** dialog is open, and only DLNA renderers on the same network are listed; see [Cast to another device](#cast-to-another-device).

### KRunner shows no Kinefin results

[Search from KRunner](#search-from-krunner) lists when Kinefin answers: the plugin on, Kinefin running and signed in, and a query of 3 characters or more.

### The picture-in-picture window falls behind other windows

Only KWin keeps the window on top; see [Keep watching while you browse](#keep-watching-while-you-browse).

## Where Kinefin keeps its files

Everything is per profile. A profile is a separate set of servers, users and settings. The default profile is `default`.

| What | Location |
| --- | --- |
| Profile list | `~/.config/kinefin/profiles.json` |
| Data, per profile | `~/.local/share/kinefin/profiles/<id>/` |
| Cache, per profile | `~/.cache/kinefin/profiles/<id>/` |
| Log | `~/.local/share/kinefin/profiles/<id>/logs/kinefin.log` |
| Downloads | `~/.local/share/kinefin/profiles/<id>/downloads/` |
| Clips staging | `~/.local/share/kinefin/profiles/<id>/clips/` |

In the Flatpak these paths are under `~/.var/app/io.github.the_beper.Kinefin/`.

The data folder also holds the settings (`kinefin.conf`), the server records (`servers.json`, without tokens), the queue and browse state, and the remembered track choices. Access tokens are in KWallet.

The log holds the newest 10 sessions: `kinefin.log` and `kinefin.log.1` to `.9`. Access tokens are never written to it. `--log-level` on the command line raises or lowers its detail for one run.

The `--profile`, `--create-profile`, `--list-profiles`, `--set-default-profile` and `--delete-profile` options manage profiles; `--help` describes them.
