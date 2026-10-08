# SneekPeek — super-lightweight command palette for Windows

One hotkey. One text box. Zero idle cost.

- Press **Ctrl+Space** → a text box appears at the top of the screen.
- Type → live **app search** (Start Menu), **web search**, **!bang search**, calculator.
- **Enter** opens the top result. Click anywhere outside (or `Esc`) → it disappears.
- Next **Ctrl+Space** → fresh empty box.
- Tray icon → **Settings… / Refresh apps / Run at startup / Quit**.
- Background: a single blocking `GetMessage` loop — **0% CPU**, **~1–3 MB RAM**, no polling, no hooks, no services.

Pure Win32 C++17, no frameworks, no installer, one ~100 KB `.exe`.

## Usage

| Input | Action |
|---|---|
| `code` | fuzzy-matches apps (`Visual Studio Code`), Enter launches |
| `anything else` | Enter → web search with your engine (default DuckDuckGo) |
| `!g cats` | Google for `cats` |
| `!yt lofi` | YouTube, `!w` Wikipedia, `!gh` GitHub, `!so` Stack Overflow, `!r` Reddit, `!m` Maps, `!t` Translate, `!a` Amazon, `!d` DuckDuckGo, `!x` X |
| `12*8` | calculator (Enter copies result) |
| `↑` `↓` | move selection, `Enter` open, `Shift+Enter` force web search, `Esc` dismiss, `F5` rescan apps |

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

Tray → **Settings…**. Stored in `%APPDATA%\SneekPeek\settings.ini`:

- Search engine (DuckDuckGo / Google / Bing / Brave / Custom URL with `%s`)
- Include `PATH` executables (more hits, more RAM — off by default)
- Run at startup (writes `HKCU\…\Run\SneekPeek`)

## Why it uses (almost) nothing

- One hidden `HWND` + blocking `GetMessage` loop → thread sleeps until an event. No timers, no polling → **0% CPU**.
- App index is a single `vector<{name, path}>` (~a few hundred entries, ~100 KB) built **once** on a background thread, then the thread exits. No icon extraction, no file watchers → **~1–3 MB** total.
- Global hotkey via `RegisterHotKey` (OS-level, no low-level keyboard hook).
- Dismiss-on-click-outside via `WM_ACTIVATE` (no mouse hook).
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
