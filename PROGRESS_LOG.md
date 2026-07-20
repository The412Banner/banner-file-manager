# Banner File Manager — Progress Log

Fork of brunodev85/wfm, rebranded as **Banner File Manager (BFM)**, shipped as `wfm.exe`
(universal x64 — runs in Box64, wowbox64, and FEXCore containers). Feature branch:
`feat/dual-pane`.

## Phase 1 — Crash fix (base)
Replaced Wine-shell32-crashing `SHFileOperation` copy/move/delete with native
`CopyFileEx`/`MoveFileEx`/`DeleteFile` + manual recursion. Rebranded. **Device-proven.**

## Phase 3 — Dual-pane split view
Refactored `content_view.c` around a per-pane `Pane` struct (both list views live at once;
notify/search resolve pane by `hwndFrom`). View ▸ Split View. Active-pane accent frame.
Copy in one pane, paste into the other. **Device-proven (render + active highlight).**

## Phase 4 — Context menu + Win11 restyle
- **Open as administrator** (`runas` verb).
- **Open with ▸** submenu: registered apps (`HKCR\Applications`) + "Choose another program…"
  (Wine Open-With dialog).
- Segoe UI font, full-row select, double-buffer.
- Hardened context-menu items to individually-allocated pointers (no use-after-realloc).

## Phase 5 — APK bake
Baked `wfm.exe` into `container_pattern_common.tzst` so the container's built-in file
manager ships our build (was reverting to stock on boot). Delivered full APK.

## Phase 6 — QoL batch
Keyboard shortcuts (F2/Del/F5/F6/Backspace/Enter/Ctrl+C·X·V·A), Show Hidden Files toggle,
byte-accurate copy **progress bar** (+ cancel), status-bar total **size**, **Properties**
dialog.

## Phase 7 — Per-pane path bars + fixes
Each pane shows its own path (active highlighted, click to activate). Fixed early
column-width/scroll issues.

## Phase 8–11 — Dark-mode cleanup (owner-draw)
Wine renders several comctl sub-controls light regardless of theme; owner-drew each dark:
column **header** (Ph8), **status bar** (Ph9), **search box** (Ph10), **navbar buttons**
(breadcrumb/go/refresh/search, Ph11). Also auto-fit columns → no horizontal scrollbar.

## Phase 12 — Theme awareness (light + dark)
The owner-drawn controls were hardcoded dark, which broke **light mode**. Added theme
detection (`isDarkMode` from window-bg luminance) and `themeFaceBg/Text/Line` +
`themeFieldBg/Text` + `themePlaceholder` helpers: dark values in dark mode, system colors
in light mode. Applied to header, status bar, search box, navbar buttons, pane labels, and
the split frame. Repaints on `WM_SYSCOLORCHANGE` for runtime theme switches.

## Known remaining
- Vertical scrollbar trough still light in dark mode (Wine scrollbar theming; only on overflow).
- Open-With could also read the file extension's own associations (OpenWithProgids/List).
- Optional: fully-editable per-pane address+search bars (currently shared top bar edits active pane).
