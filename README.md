# SneekPeek — super-lightweight command palette for Windows

One hotkey. One text box. Zero idle cost.

- Press **Ctrl+Space** → only a search bar appears (nothing else until you type).
- Type → fuzzy **app search** (Start Menu, exe-only, plus Store apps), **system settings pages**, **power/awake/bang/quick chips**, **web search**, calculator — grouped under one header per section.
- **Enter** always runs the top *actionable* result (never a section heading — with nothing selected it searches the typed text) — the palette hides instantly, before the app even opens. Click anywhere outside (or `Esc`) → it disappears. Failed launches report a tray balloon; clicking it reopens the palette to retry.
- Next **Ctrl+Space** → fresh empty bar.
- Tray icon → **Settings… / Refresh apps / Run at startup / Quit**.
- Background: a single blocking `GetMessage` loop — **0% CPU**, **~1–3 MB RAM** at idle, no polling, no hooks, no services.

Pure Win32 C++17, no frameworks, no installer, one ~100 KB `.exe`.

## Usage

| Input | Action |
|---|---|
| `code` | fuzzy-matches apps (`Visual Studio Code`), Enter launches |
| `anything else` | Enter → web search with your engine (default DuckDuckGo) |
| `google.com` | URL-shaped input opens the site directly (scheme optional, `https` assumed) |
| `yt` | matching site bangs appear as chips above the list — Enter arms the chip (`yt` + `Space` does the same): just the pill in the bar, no list — type the query, Enter searches (Backspace on empty query or click `x` exits) |
| `!g cats` | one-shot bang without the chip (Google for `cats`) |
| `shu` | power chips appear above the list (Shut down, Restart, Sleep, Hibernate) — Enter runs the selected chip (Shut down/Restart force-close apps first, unsaved work is lost — hence the confirm), Tab cycles, Down jumps to the list |
| `awake` | `Awake: On/Off` chip — keeps display + system awake without touching power settings; persists across restarts, also in the tray menu |
| `bluetooth` | matching Windows Settings pages under their own header — Enter opens the page |
| `gmail` | quick-link chips (fixed URLs, no query) work the same way — Enter opens |
| `12*8` | calculator (Enter copies result; undefined math like `1/0` shows nothing) |
| `:` + `Space` | names-only file browser (hidden items excluded): drives show as `C:\` (bare letters: volume names block on drives that don't answer, e.g. an empty DVD or an offline share) — type to filter, `Space`/`Enter` on a folder drills in (stays open), `Space`/`Enter` on a file opens it with the default app; slim bottom bar + `Shift+Enter` opens the current folder in Explorer; long lists scroll with `↑` `↓` or mouse wheel (no scrollbar); grid view by default; `..` / empty-box `Backspace` goes up, `x` or drive-level `Backspace` exits — one folder scanned at a time, no sizes/icons/recursion |
| `↑` `↓` | move selection (mirrors into the box without refiltering; wraps past either end back to your typed text; Up from the first row jumps back to the chips), `Enter` open, `Shift+Enter` force web search, `Esc` dismiss, `F5` rescan apps |
| Right-click | dark context menu on any row (Open / Run as administrator / Open file location / Copy path / Properties / Hide app…, per-type: Open with…, Eject, Extract all…, engine switch), palette menu on headers and empty areas, same theme as the tray menu |
| Drag & drop | drag files, folders, drives, or classic app targets out of the list/grid into Explorer (copy-only); drop a folder onto the palette to browse it, drop a file to paste its path |

Bang aliases are editable in Settings (default: `yt`/`youtube`, `gh`/`github`, `w`/`wiki`, `r`, `m`, `t`, `a`, `x`, `so`, `d`, `g` — each with its own color).

## Build

You need a C++ compiler (MSVC **or** MinGW — either works).

```bat
build.bat
```

That produces `SneekPeek.exe`. Or with CMake:

```bat
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
```

The last line runs the unit tests (calculator incl. divide-by-zero and `-2^2` precedence, fuzzy ranking, bang parsing, URL detection, JSON decoding, settings caps, Enter-key decision table, file-browse helpers + single-level scan, settings INI round-trip through the real profile API, drive ranking, and the extension→glyph table).

No compiler yet? One line (then reopen the terminal and re-run `build.bat`):

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

Double-click `SneekPeek.exe` — it lives in the tray. Press **Ctrl+Space**.

> If `Ctrl+Space` does nothing, another app owns it (common culprits: IME /
> language switcher, PowerToys, other launchers). SneekPeek shows a warning
> in that case — free the hotkey elsewhere, then restart SneekPeek.

## Get the .exe from GitHub Actions (no compiler needed)

1. Push this folder to your GitHub repo:
   ```powershell
   git init
   git add .
   git commit -m "SneekPeek v1"
   git branch -M main
   git remote add origin https://github.com/<you>/<repo>.git
   git push -u origin main
   ```
2. Open the repo → **Actions** tab → **Build SneekPeek (Windows)** → latest green run → **Artifacts** → download **SneekPeek-windows-x64** → unzip → `SneekPeek.exe`.
3. Double-click it, press **Ctrl+Space**.

Notes:

- Downloading artifacts requires being logged into GitHub.
- For a login-free direct link, tag a release (`git tag v1.0.0` + `git push origin v1.0.0`) — the **Release SneekPeek** workflow attaches `SneekPeek.exe` to a GitHub Release.
- The exe is unsigned, so SmartScreen may warn on first run → More info → Run anyway.

## Settings

Tray → **Settings…** (tabbed, dark, matches the app — every change saves instantly, no Save button). Stored in `%APPDATA%\SneekPeek\settings.ini`, written as one UTF-16 file via a temp file + rename (so an interrupted save can't corrupt it):

- **General** — engine (DuckDuckGo / Google / Bing / Brave / Custom URL with `%s`), browser for web + bang + quick links (System default or picked Chrome/Edge/Firefox/Brave/Opera/Vivaldi/custom `.exe`)
- **Files** — file browser view (List/Grid, grid by default), grid rows (1–4) and columns (2–7); file mode only, applies instantly
- **Hidden apps** — checkbox list of every indexed app; checked ones never appear in suggestions
- **Bangs** — table (Aliases / Name / URL, `https://` or `http://` only): Add appends an editable row, double-click any cell to edit in place, Del or Remove deletes rows, colors auto-picked
- **Quick links** — table (Name / URL, `https://` or `http://` only) with the same editing; typing the name in the palette shows a chip that opens the fixed URL on Enter (e.g. Gmail)
- Every settings change saves instantly with a `Saved` timestamp; layout and fonts follow the monitor DPI, and Settings opens on the monitor you're using

## Why it uses (almost) nothing

- One hidden `HWND` + blocking `GetMessage` loop → thread sleeps until an event. No timers, no polling → **0% CPU**.
- App index is a single `vector<{name, path}>` with cached lowercase names, built **once** on a background thread (Start Menu `.lnk` targets resolved to keep exe-tools only, Store apps via `shell:AppsFolder` ×2 COM routes plus `Get-StartApps`), then the thread exits. The finished index is cached to disk, so cold starts search instantly and only refresh quietly in the background. No icon extraction, no file watchers → **~1–3 MB** total.
- Typing stays cheap: ranking only sorts the rows it will actually show, lowercase names are computed once per list rather than inside sort comparators, and glyph pens/brushes are memoized so a repaint doesn't churn GDI objects.
- Settings persist as a single file write (not one write per key), so a save no longer re-reads and rewrites the whole INI hundreds of times.
- Global hotkey via `RegisterHotKey` (OS-level, no low-level keyboard hook).
- Dismiss-on-click-outside via `WM_ACTIVATE` + `WM_ACTIVATEAPP` (no mouse hook).
- `WS_EX_TOOLWINDOW` → no taskbar button; single instance via mutex.

## Project layout

```
src/main.cpp       window, hotkey, tray, palette UI, execution  (WinMain)
src/app_index.h/.cpp  Start Menu scan + fuzzy rank (lazy, one-shot)
src/bangs.h        !bang table + URL encoding (header-only)
src/files.h        single-level file browser: names + count only (header-only)
src/icons.h        predefined kind/extension glyphs, drawn not loaded (header-only)
src/settings.h     INI load/save + Run-at-startup (header-only)
src/app.ico        app/tray icon (multi-size, built from search.png)
src/app.rc         icon + version info resources
src/resource.h     resource IDs
search.png         icon source (rerun tools/make_ico.ps1 after replacing)
CMakeLists.txt     CMake build (MSVC + MinGW)
build.bat          zero-dependency build (cl or g++)
.github/workflows/  CI: build.yml (Actions artifact) + release.yml (tag → Release)
```

## License

MIT — do anything, no warranty.
