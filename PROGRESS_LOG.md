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

## Phase 13 — Dark non-client scrollbars (post-1.0.0)
Branch `fix/dark-scrollbars`, CI green run `29817440567`, build **297984 B**.
Device screenshots showed white bars under the content list in **both** single and split
view (and down the right edge) — the last known-light element, previously deferred.
Cause: Wine paints a window's own (non-client) scrollbars with the classic light 3D look
regardless of the container theme, and there is no message to recolour them
(`WM_CTLCOLORSCROLLBAR` only covers standalone scrollbar controls).
- `main.c`: `themePaintScrollbars(HWND)` repaints a window's scrollbars over its window DC —
  trough RGB(38,38,38), thumb RGB(95,95,95), owner-drawn arrow glyphs, plus the corner square
  where a horizontal and a vertical bar meet. Geometry from `GetScrollBarInfo`, with a
  `GetScrollInfo` fallback if Wine reports no usable thumb. Light mode returns early
  (system rendering already matches).
- `themeScrollbarsNeedRepaint(UINT)` whitelist: WM_NCPAINT alone is not enough because Wine
  draws from inside the control's own handling (`SetScrollInfo` paints immediately) and from
  the thumb-drag modal loop. Subclasses repaint after layout/scroll/focus messages and a few
  LVM_/TVM_ messages; hot query messages are excluded so nothing repaints in a loop.
- `content_view.c` `ContentViewWndProc` and a new thin `TreeviewWndProc` subclass in
  `treeview.c` (the tree was not subclassed before) call it after the original proc.

### 1.1.0 release
Phase 13 merged to `main` (fast-forward from `fc2e6aa`). `APP_VERSION` -> **1.1.0**.
Also in this release: the shared UI font dropped a step (Segoe UI -15 -> -14).
