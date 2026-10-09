# SneekPeek — super-lightweight command palette for Windows

One hotkey. One text box. Zero idle cost.

- Press **Ctrl+Space** → only a search bar appears (nothing else until you type).
- Type → fuzzy **app search** (Start Menu, exe-only, plus Store apps), **system settings pages**, **power/awake/bang/quick chips**, **web search** with live **online suggestions** (<0.5s), calculator — grouped under one header per section.
- **Enter** runs the top result — the palette hides instantly, before the app even opens. Click anywhere outside (or `Esc`) → it disappears.
- Next **Ctrl+Space** → fresh empty bar.
- Tray icon → **Settings… / Refresh apps / Run at startup / Quit**.
- Background: a single blocking `GetMessage` loop — **0% CPU**, **~1–3 MB RAM**, no polling, no hooks, no services.

Pure Win32 C++17, no frameworks, no installer, one ~100 KB `.exe`.

## Usage

| Input | Action |
|---|---|
| `code` | fuzzy-matches apps (`Visual Studio Code`), Enter launches |
| `anything else` | Enter → web search with your engine (default DuckDuckGo) |
| `google.com` | URL-shaped input opens the site directly (scheme optional, `https` assumed) |
| `yt` | matching site bangs appear as chips above the list — Enter arms the chip (`yt` + `Space` does the same): just the pill in the bar, no list — type the query, Enter searches (Backspace on empty query or click `x` exits) |
| `!g cats` | one-shot bang without the chip (Google for `cats`) |
| `shu` | power chips appear above the list (Shut down, Restart, Sleep, Hibernate) — Enter runs the selected chip, Tab cycles, Down jumps to the list |
| `awake` | `Awake: On/Off` chip — keeps display + system awake without touching power settings; persists across restarts, also in the tray menu |
| `bluetooth` | matching Windows Settings pages under their own header — Enter opens the page |
| `gmail` | quick-link chips (fixed URLs, no query) work the same way — Enter opens |
| `12*8` | calculator (Enter copies result) |
| `↑` `↓` | move selection (mirrors into the box without refiltering; wraps past either end back to your typed text; Up from the first row jumps back to the chips), `Enter` open, `Shift+Enter` force web search, `Esc` dismiss, `F5` rescan apps |

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
```

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

Tray → **Settings…** (tabbed, dark, matches the app — every change saves instantly, no Save button). Stored in `%APPDATA%\SneekPeek\settings.ini`:

- **General** — engine (DuckDuckGo / Google / Bing / Brave / Custom URL with `%s`), browser for web + bang + quick links (System default or picked Chrome/Edge/Firefox/Brave/Opera/Vivaldi/custom `.exe`)
- **Hidden apps** — checkbox list of every indexed app; checked ones never appear in suggestions
- **Bangs** — table (Aliases / Name / URL): Add appends an editable row, double-click any cell to edit in place, Del removes rows, colors auto-picked
- **Quick links** — table (Name / URL) with the same editing; typing the name in the palette shows a chip that opens the fixed URL on Enter (e.g. Gmail)

## Why it uses (almost) nothing

- One hidden `HWND` + blocking `GetMessage` loop → thread sleeps until an event. No timers, no polling → **0% CPU**.
- App index is a single `vector<{name, path}>` with cached lowercase names, built **once** on a background thread (Start Menu `.lnk` targets resolved to keep exe-tools only, Store apps via `shell:AppsFolder` ×2 COM routes plus `Get-StartApps`), then the thread exits. No icon extraction, no file watchers → **~1–3 MB** total.
- Global hotkey via `RegisterHotKey` (OS-level, no low-level keyboard hook).
- Dismiss-on-click-outside via `WM_ACTIVATE` + `WM_ACTIVATEAPP` (no mouse hook).
- `WS_EX_TOOLWINDOW` → no taskbar button; single instance via mutex.

## Project layout

```
src/main.cpp       window, hotkey, tray, palette UI, execution  (WinMain)
src/app_index.h/.cpp  Start Menu scan + fuzzy rank (lazy, one-shot)
src/bangs.h        !bang table + URL encoding (header-only)
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
