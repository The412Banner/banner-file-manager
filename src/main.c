#include "main.h"

static const wchar_t mainWndClass[] = L"WFM-MainWnd";

extern struct FileNode* currPathFileNode;
extern HWND hwndNavbar;
extern HWND hwndSizebar;
extern HWND hwndStatusbar;
extern HWND hwndToolbar;
extern HWND hwndTreeview;

HINSTANCE globalHInstance = NULL;
HWND hwndMain = NULL;
struct LC_STR lc_str = {0};

// Dual-pane layout: each pane's list view sits inset by PANE_FRAME inside its cell;
// the surrounding band is painted accent (active) or background (inactive) in WM_PAINT.
#define PANE_FRAME 2
static RECT paneCell[2] = {0};
static HMENU hViewMenu = NULL;
static HBRUSH paneLabelActiveBrush = NULL;
static HBRUSH paneLabelInactiveBrush = NULL;
static bool paneLabelInactiveDark = false;

void cvInvalidatePaneFrames(void) {
    for (int i = 0; i < 2; i++) InvalidateRect(hwndMain, &paneCell[i], TRUE);
}

// Theme awareness: our owner-drawn controls (header, status bar, navbar buttons,
// search box) must follow the container's light/dark theme instead of being hardcoded.
// Dark is detected from the window background luminance; in light mode we defer to the
// system colors so the controls match the rest of the (light) UI.
bool isDarkMode(void) {
    DWORD c = GetSysColor(COLOR_WINDOW);
    return ((GetRValue(c) + GetGValue(c) + GetBValue(c)) / 3) < 128;
}
COLORREF themeFaceBg(void)    { return isDarkMode() ? RGB(45, 45, 45)    : GetSysColor(COLOR_BTNFACE); }
COLORREF themeFaceText(void)  { return isDarkMode() ? RGB(225, 225, 225) : GetSysColor(COLOR_BTNTEXT); }
COLORREF themeFaceLine(void)  { return isDarkMode() ? RGB(70, 70, 70)    : GetSysColor(COLOR_BTNSHADOW); }
COLORREF themeFieldBg(void)   { return isDarkMode() ? RGB(45, 45, 45)    : GetSysColor(COLOR_WINDOW); }
COLORREF themeFieldText(void) { return isDarkMode() ? RGB(230, 230, 230) : GetSysColor(COLOR_WINDOWTEXT); }
COLORREF themePlaceholder(void){ return isDarkMode() ? RGB(150, 150, 150) : GetSysColor(COLOR_GRAYTEXT); }

// Shared modern UI font (Segoe UI). Larger than the classic GUI font so rows are
// taller and easier to hit on a touchscreen, and reads more like Windows 11.
static HFONT uiFont = NULL;
HFONT getUIFont(void) {
    if (!uiFont) {
        uiFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    return uiFont;
}

void GetWindowRectInParent(HWND hwnd, RECT* rect) {
    GetWindowRect(hwnd, rect);
    MapWindowPoints(HWND_DESKTOP, GetParent(hwnd), (LPPOINT)rect, 2);
}

INT_PTR CALLBACK AboutDialogProc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {      
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDOK:
                case IDCANCEL: {
                  EndDialog(hwndDlg, (INT_PTR) LOWORD(wParam));
                  return (INT_PTR) TRUE;
                }
            }
            break;
        }
        case WM_INITDIALOG: {
            RECT rect, rect1;
            GetWindowRect(GetParent(hwndDlg), &rect);
            GetClientRect(hwndDlg, &rect1);
            SetWindowPos(hwndDlg, NULL, (rect.right + rect.left) / 2 - (rect1.right - rect1.left) / 2, (rect.bottom + rect.top) / 2 - (rect1.bottom - rect1.top) / 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
            
            SetWindowText(hwndDlg, lc_str.about);
            SetWindowText(GetDlgItem(hwndDlg, IDC_APP_NAME), lc_str.app_name);
            SetWindowText(GetDlgItem(hwndDlg, IDC_APP_VERSION), lc_str.app_version);
            SetWindowText(GetDlgItem(hwndDlg, IDC_APP_DEV_NAME), lc_str.app_dev_name);
            return (INT_PTR)TRUE;
        }
    }

    return (INT_PTR)FALSE;
}

void mainMenuCommand(WPARAM wParam) {
    switch (LOWORD(wParam)) {
        case ID_EDIT_CUT:
            onMenuItemCutClick();
            break;
        case ID_EDIT_COPY:
            onMenuItemCopyClick();
            break;
        case ID_EDIT_PASTE:
            onMenuItemPasteClick();
            break;
        case ID_EDIT_PASTE_SHORTCUT:
            onMenuItemPasteShortcutClick();
            break;
        case ID_EDIT_SELECT_ALL:
            onMenuItemSelectAllClick();
            break;                  
        case ID_HELP_ABOUT:
            DialogBox(globalHInstance, MAKEINTRESOURCE(IDD_ABOUT), hwndMain, &AboutDialogProc);
            break;
        case ID_FILE_EXIT:
            DestroyWindow(hwndMain);
            break;
        case ID_VIEW_LARGEICONS:
            setViewStyle(STYLE_LARGE_ICON);
            break;
        case ID_VIEW_SMALLICONS:
            setViewStyle(STYLE_SMALL_ICON);
            break;
        case ID_VIEW_LIST:
            setViewStyle(STYLE_LIST);
            break;
        case ID_VIEW_DETAILS:
            setViewStyle(STYLE_DETAILS);
            break;
        case ID_VIEW_SPLIT:
            cvToggleSplit();
            break;
        case ID_VIEW_HIDDEN:
            showHiddenFiles = !showHiddenFiles;
            if (hViewMenu) CheckMenuItem(hViewMenu, ID_VIEW_HIDDEN, MF_BYCOMMAND | (showHiddenFiles ? MF_CHECKED : MF_UNCHECKED));
            navigateRefresh();
            break;
    }
}

void resizeControls() {
    RECT rect;
    GetClientRect(hwndMain, &rect);

    RECT toolbarRect;
    RECT buttonRect;
    SendMessage(hwndToolbar, TB_GETITEMRECT, 0, (LPARAM)&buttonRect);
    SetWindowPos(hwndToolbar, NULL, 0, 0, rect.right, buttonRect.bottom + 3, SWP_NOZORDER);
    GetWindowRectInParent(hwndToolbar, &toolbarRect);

    RECT statusbarRect;
    SendMessage(hwndStatusbar, WM_SIZE, 0, 0);
    GetWindowRectInParent(hwndStatusbar, &statusbarRect);

    RECT navbarRect;
    int navbarHeight = getNavbarHeight();
    SetWindowPos(hwndNavbar, NULL, 0, toolbarRect.bottom, rect.right, navbarHeight, SWP_NOZORDER);
    GetWindowRectInParent(hwndNavbar, &navbarRect);
    
    RECT treeviewRect;
    GetWindowRectInParent(hwndTreeview, &treeviewRect);
    int treeviewHeight = statusbarRect.top - navbarRect.bottom;
    SetWindowPos(hwndTreeview, NULL, 0, navbarRect.bottom, treeviewRect.right, treeviewHeight, SWP_NOZORDER);
    
    const int sizebarWidth = 5;
    SetWindowPos(hwndSizebar, NULL, treeviewRect.right, navbarRect.bottom, sizebarWidth, treeviewHeight, SWP_NOZORDER);

    int contentViewX = treeviewRect.right + sizebarWidth;
    int contentY = navbarRect.bottom;
    int contentW = rect.right - contentViewX;
    int contentH = treeviewHeight;

    bool split = cvSplitOn();
    int inset = split ? PANE_FRAME : 0;

    if (split) {
        const int gap = 6;
        int half = (contentW - gap) / 2;
        paneCell[0] = (RECT){contentViewX, contentY, contentViewX + half, contentY + contentH};
        paneCell[1] = (RECT){contentViewX + half + gap, contentY, contentViewX + contentW, contentY + contentH};
    }
    else {
        paneCell[0] = (RECT){contentViewX, contentY, contentViewX + contentW, contentY + contentH};
        paneCell[1] = (RECT){0, 0, 0, 0};
    }

    int nPanes = split ? 2 : 1;
    int labelH = split ? 20 : 0;
    for (int i = 0; i < nPanes; i++) {
        RECT c = paneCell[i];
        int x = c.left + inset, y = c.top + inset;
        int w = (c.right - c.left) - 2 * inset;
        int h = (c.bottom - c.top) - 2 * inset;
        if (split) {
            SetWindowPos(cvPaneLabel(i), NULL, x, y, w, labelH, SWP_NOZORDER);
        }
        SetWindowPos(cvPaneHwnd(i), NULL, x, y + labelH, w, h - labelH, SWP_NOZORDER);
        cvFitColumns(cvPaneHwnd(i), w);
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) { 
        case WM_SIZE: {
            resizeControls();
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (cvSplitOn()) {
                HBRUSH accent = CreateSolidBrush(RGB(0, 120, 215));
                HBRUSH normal = CreateSolidBrush(themeFaceBg());
                for (int i = 0; i < 2; i++) {
                    // Children are clipped (WS_CLIPCHILDREN), so this only paints the
                    // PANE_FRAME band around each list view.
                    FillRect(hdc, &paneCell[i], (i == cvActiveIdx()) ? accent : normal);
                }
                DeleteObject(accent);
                DeleteObject(normal);
            }
            EndPaint(hwnd, &ps);
            break;
        }
        case WM_SYSCOLORCHANGE: {
            // Container theme switched (light <-> dark): repaint everything with new colors.
            RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HWND ctl = (HWND)lParam;
            for (int i = 0; i < 2; i++) {
                if (ctl == cvPaneLabel(i)) {
                    bool dark = isDarkMode();
                    if (!paneLabelActiveBrush) paneLabelActiveBrush = CreateSolidBrush(RGB(0, 120, 215));
                    if (!paneLabelInactiveBrush || dark != paneLabelInactiveDark) {
                        if (paneLabelInactiveBrush) DeleteObject(paneLabelInactiveBrush);
                        paneLabelInactiveBrush = CreateSolidBrush(themeFaceBg());
                        paneLabelInactiveDark = dark;
                    }
                    HDC hdc = (HDC)wParam;
                    bool active = (i == cvActiveIdx());
                    SetBkMode(hdc, OPAQUE);
                    SetTextColor(hdc, active ? RGB(255, 255, 255) : themeFaceText());
                    SetBkColor(hdc, active ? RGB(0, 120, 215) : themeFaceBg());
                    return (LRESULT)(active ? paneLabelActiveBrush : paneLabelInactiveBrush);
                }
            }
            break;
        }
        case WM_COMMAND: {
            if (lParam == 0 && HIWORD(wParam) == 0) {
                mainMenuCommand(wParam);
                return 0;
            }
            else if ((HWND)lParam == hwndToolbar) {
                toolbarCommand(LOWORD(wParam));
            }
            else if (HIWORD(wParam) == STN_CLICKED && cvActivatePaneByLabel((HWND)lParam)) {
                return 0;
            }
            break;
        }
        case WM_SYSCOMMAND: {
            switch (LOWORD(wParam)) {
                case ID_HELP_ABOUT: {
                    DialogBox(globalHInstance, MAKEINTRESOURCE(IDD_ABOUT), hwnd, &AboutDialogProc);
                    return 0;
                }
            }
            break;
        }
        case WM_CLOSE: {
            if (MessageBox(NULL, lc_str.msg_confirm_exit_app, lc_str.confirm_exit, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                PostQuitMessage(0);
            }
            return 0;
        }       
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
        case WM_NOTIFY: {
            NMHDR* nmhdr = (NMHDR*)lParam;
            if (cvIsContentView(nmhdr->hwndFrom)) {
                return contentViewNotify(nmhdr);
            }
            else if (nmhdr->hwndFrom == hwndTreeview) {
                return treeviewNotify(nmhdr);
            }
            else return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void navigateToFileNode(struct FileNode* node) {
    if (node) {
        clearAddrButtons();
        clearContentView();
        setCurrPathFileNode(node);
        navigateRefresh();
    }
}

void navigateToPath(wchar_t* path) {
    if (path) {
        clearAddrButtons();
        clearContentView();   
        setCurrPathFromString(path);
        navigateRefresh();
    }
}

void navigateUp() {
    if (currPathFileNode->parent) {
        clearAddrButtons();
        clearContentView();
        setCurrPathFileNode(currPathFileNode->parent);
        navigateRefresh();
    }
}

void navigateRefresh() {
    if (currPathFileNode) {
        buildChildNodes(currPathFileNode, false);
        SetWindowText(hwndMain, currPathFileNode->name);    
        updateAddrButtons();
        refreshContentView();
    }
}

void openFileNode(struct FileNode* node) {
    if (node->type == TYPE_FILE) {
        wchar_t path[MAX_PATH] = {0};
        wchar_t parentPath[MAX_PATH] = {0};
        getFileNodePath(node, path);
        getFileNodePath(node->parent, parentPath);
        ShellExecute(hwndMain, L"open", path, NULL, parentPath, SW_SHOW);
    }
    else navigateToFileNode(node);
}

static void createMainMenu() {
    HMENU hmFile = CreatePopupMenu();
    AppendMenu(hmFile, MF_STRING, ID_FILE_EXIT, lc_str.exit);
    
    HMENU hmEdit = CreatePopupMenu();
    AppendMenu(hmEdit, MF_STRING, ID_EDIT_CUT, lc_str.cut);
    AppendMenu(hmEdit, MF_STRING, ID_EDIT_COPY, lc_str.copy);
    AppendMenu(hmEdit, MF_STRING, ID_EDIT_PASTE, lc_str.paste);
    AppendMenu(hmEdit, MF_STRING, ID_EDIT_PASTE_SHORTCUT, lc_str.paste_shortcut);
    AppendMenu(hmEdit, MF_SEPARATOR, 0, NULL);
    AppendMenu(hmEdit, MF_STRING, ID_EDIT_SELECT_ALL, lc_str.select_all);
    
    HMENU hmView = CreatePopupMenu();
    AppendMenu(hmView, MF_STRING, ID_VIEW_LARGEICONS, lc_str.large_icons);
    AppendMenu(hmView, MF_STRING, ID_VIEW_SMALLICONS, lc_str.small_icons);
    AppendMenu(hmView, MF_STRING, ID_VIEW_LIST, lc_str.list);
    AppendMenu(hmView, MF_STRING, ID_VIEW_DETAILS, lc_str.details);
    AppendMenu(hmView, MF_SEPARATOR, 0, NULL);
    AppendMenu(hmView, MF_STRING, ID_VIEW_SPLIT, lc_str.split_view);
    AppendMenu(hmView, MF_STRING, ID_VIEW_HIDDEN, lc_str.show_hidden);
    hViewMenu = hmView;

    HMENU hmHelp = CreatePopupMenu();
    AppendMenu(hmHelp, MF_STRING, ID_HELP_ABOUT, lc_str.about);
    
    HMENU hmMain = CreateMenu();
    AppendMenu(hmMain, MF_POPUP, (UINT_PTR)hmFile, lc_str.file);
    AppendMenu(hmMain, MF_POPUP, (UINT_PTR)hmEdit, lc_str.edit);
    AppendMenu(hmMain, MF_POPUP, (UINT_PTR)hmView, lc_str.view);
    AppendMenu(hmMain, MF_POPUP, (UINT_PTR)hmHelp, lc_str.help);
    
    SetMenu(hwndMain, hmMain);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nCmdShow) {
    int numArgs;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &numArgs);
    
    wchar_t localeName[16] = {0};
    GetSystemDefaultLocaleName(localeName, 16);
    
    loadLCStrings(localeName);

    globalHInstance = hInstance;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEX wcx = {0};
    wcx.cbSize = sizeof(wcx);
    wcx.style = CS_HREDRAW | CS_VREDRAW;
    wcx.lpfnWndProc = &MainWndProc;
    wcx.cbClsExtra = 0;
    wcx.cbWndExtra = 0;
    wcx.hInstance = hInstance;
    wcx.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MAIN));
    wcx.hCursor = LoadCursor(hInstance, IDC_ARROW);
    wcx.hbrBackground = (HBRUSH)COLOR_WINDOW;
    wcx.lpszClassName = mainWndClass;
    wcx.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MAIN));

    if (!RegisterClassEx(&wcx)) return 0;
    
    initFileNodes();

    HWND hwndDesktop = GetDesktopWindow();
    RECT desktopRect;
    GetWindowRect(hwndDesktop, &desktopRect);
    int hwndWidth = (desktopRect.right - desktopRect.left) * 0.8f;
    int hwndHeight = (desktopRect.bottom - desktopRect.top) * 0.8f;
    
    hwndMain = CreateWindowEx(0, mainWndClass, L"", WS_CLIPCHILDREN | WS_OVERLAPPEDWINDOW, 
                              0, 0, hwndWidth, hwndHeight, NULL, NULL, hInstance, NULL);
    if (!hwndMain) return 0;
    
    createMainMenu();
    createToolbar();
    createNavbar();
    createTreeview();
    createSizebar();
    createContentView();
    cvInitPanePaths();
    createStatusbar();

    setViewStyle(STYLE_DETAILS);
    int treeviewWidth = hwndWidth * 0.2f;
    SetWindowPos(hwndTreeview, NULL, 0, 0, treeviewWidth, 0, SWP_NOZORDER | SWP_NOMOVE);    
    
    if (numArgs > 1) {
        navigateToPath(args[1]);
    }
    else navigateRefresh();

    ShowWindow(hwndMain, SW_SHOW);
    UpdateWindow(hwndMain);

    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    return (int)msg.wParam;
}
