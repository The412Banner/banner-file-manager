#include "main.h"

#define COLUMN_NAME_IDX 0
#define COLUMN_TYPE_IDX 1
#define COLUMN_SIZE_IDX 2
#define COLUMN_DATE_IDX 3
#define COLUMN_PATH_IDX 4

#define NUM_PANES 2

enum Msg {
    MSG_ADD_ITEM = WM_APP,
    MSG_SEARCH_DONE
};

enum ContextMenuType {
    MENU_SINGLE,
    MENU_MULTIPLE,
    MENU_EMPTY
};

struct ListItem {
    int icon;
    struct FileNode* node;
    wchar_t type[64];
    wchar_t formattedSize[32];
    wchar_t formattedDate[32];
    bool loaded;
    uint64_t size;
    wchar_t* path;
    FILETIME modifiedTime;
};

// Per-pane state. Both list views are live simultaneously (each fires its own
// LVN_GETDISPINFO on repaint), so the backing data must be resolvable per HWND.
struct Pane {
    HWND hwndList;
    struct FileNode* currPath;      // this pane's path chain (independent per pane)
    struct ListItem* items;
    int numItems;
    enum ViewStyle viewStyle;
    char sortColumnIdx;
    bool sortAscending;
    struct SearchData* searchData;
};

struct SearchData {
    wchar_t* keyword;
    bool active;
    bool canceled;
    struct Pane* pane;
};

struct ContextMenuItem {
    wchar_t* text;
    void(*proc)();
    wchar_t* cmdData;       // legacy: run via cmd /C
    wchar_t* openExe;       // "Open with" app: ShellExecute this exe with openFile
    wchar_t* openFile;      // the file to hand to openExe (heap; freed with the item)
};

static void onMenuItemLoadISOImageClick();
static void onMenuItemUnloadISOImageClick();

static struct ContextMenuItem cmiOpen = {NULL, &onMenuItemOpenClick, NULL};
static struct ContextMenuItem cmiEdit = {NULL, &onMenuItemEditClick, NULL};
static struct ContextMenuItem cmiCut = {NULL, &onMenuItemCutClick, NULL};
static struct ContextMenuItem cmiCopy = {NULL, &onMenuItemCopyClick, NULL};
static struct ContextMenuItem cmiCreateShortcut = {NULL, &onMenuItemCreateShortcutClick, NULL};
static struct ContextMenuItem cmiDelete = {NULL, &onMenuItemDeleteClick, NULL};
static struct ContextMenuItem cmiRename = {NULL, &onMenuItemRenameClick, NULL};
static struct ContextMenuItem cmiPaste = {NULL, &onMenuItemPasteClick, NULL};
static struct ContextMenuItem cmiPasteShortcut = {NULL, &onMenuItemPasteShortcutClick, NULL};
static struct ContextMenuItem cmiNewFolder = {NULL, &onMenuItemNewFolderClick, NULL};
static struct ContextMenuItem cmiNewFile = {NULL, &onMenuItemNewFileClick, NULL};
static struct ContextMenuItem cmiLoadISOImage = {NULL, &onMenuItemLoadISOImageClick, NULL};
static struct ContextMenuItem cmiUnloadISOImage = {NULL, &onMenuItemUnloadISOImageClick, NULL};
static struct ContextMenuItem cmiOpenAsAdmin = {NULL, &onMenuItemOpenAsAdminClick, NULL};
static struct ContextMenuItem cmiChooseProgram = {NULL, &onMenuItemOpenWithClick, NULL};

static WNDPROC OrigWndProc;
static HMENU hContextMenu;

static struct Pane panes[NUM_PANES] = {0};
static int activeIdx = 0;
static bool splitOn = false;
static struct Pane* g_sortPane = NULL;

static struct FileNode** selectedItems = NULL;
static int numSelectedItems = 0;

static struct ContextMenuItem** menuItems = NULL;
static int numMenuItems = 0;

// Append a fresh, individually-allocated context-menu item. Returning a stable pointer
// (rather than &menuItems[i]) keeps menu dwItemData valid across later reallocs.
static struct ContextMenuItem* addMenuItemSlot() {
    int index = numMenuItems++;
    menuItems = realloc(menuItems, numMenuItems * sizeof(struct ContextMenuItem*));
    struct ContextMenuItem* it = calloc(1, sizeof(struct ContextMenuItem));
    menuItems[index] = it;
    return it;
}

extern struct FileNode* currPathFileNode;
extern HINSTANCE globalHInstance;
extern HWND hwndMain;

// forward declarations (defined later in this file / in main.c)
static void refreshPane(struct Pane* p);
static void cvSetActiveByHwnd(HWND h);
void cvInvalidatePaneFrames(void); // main.c

static struct Pane* activePane() {
    return &panes[activeIdx];
}

static struct Pane* paneFromHwnd(HWND h) {
    for (int i = 0; i < NUM_PANES; i++) {
        if (panes[i].hwndList == h) return &panes[i];
    }
    return &panes[activeIdx];
}

// ---- exported accessors used by main.c / navbar.c ----
HWND cvActiveHwnd() {
    return panes[activeIdx].hwndList;
}

HWND cvPaneHwnd(int i) {
    return (i >= 0 && i < NUM_PANES) ? panes[i].hwndList : NULL;
}

int cvActiveIdx() {
    return activeIdx;
}

bool cvSplitOn() {
    return splitOn;
}

bool cvIsContentView(HWND h) {
    for (int i = 0; i < NUM_PANES; i++) {
        if (panes[i].hwndList == h) return true;
    }
    return false;
}

static void fillFileInfo(struct FileNode* node, struct ListItem* item) {
    item->size = 0;
    memset(&item->modifiedTime, 0, sizeof(FILETIME));

    if (node->type == TYPE_FILE) {
        LARGE_INTEGER filesize;
        WIN32_FILE_ATTRIBUTE_DATA info = {0};

        wchar_t path[MAX_PATH] = {0};
        getFileNodePath(node, path);
        GetFileAttributesEx(path, GetFileExInfoStandard, &info);

        if ((info.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)) {
            filesize.LowPart = info.nFileSizeLow;
            filesize.HighPart = info.nFileSizeHigh;
            item->size = filesize.QuadPart;
        }

        memcpy(&item->modifiedTime, &info.ftLastWriteTime, sizeof(FILETIME));
    }
}

static void updateStatusbar(struct Pane* p) {
    // The single status bar reflects the active pane only.
    if (p != activePane()) return;
    wchar_t statusText[32] = {0};
    swprintf_s(statusText, 32, L"%d %ls", p->numItems, lc_str.items);
    setStatusbarText(statusText);
}

static void freeMenuItems() {
    if (menuItems) {
        for (int i = 0; i < numMenuItems; i++) {
            struct ContextMenuItem* it = menuItems[i];
            if (it->cmdData) free(it->cmdData);
            if (it->openExe) {
                free(it->openExe);
                if (it->openFile) free(it->openFile);
                if (it->text) free(it->text);
            }
            free(it);
        }
        free(menuItems);
        menuItems = NULL;
    }
    numMenuItems = 0;
}

static void clearPane(struct Pane* p) {
    ListView_SetItemCountEx(p->hwndList, 0, 0);
    ListView_DeleteColumn(p->hwndList, COLUMN_PATH_IDX);

    if (p->items) {
        for (int i = 0; i < p->numItems; i++) {
            if (p->items[i].path) {
                free(p->items[i].path);
                p->items[i].path = NULL;
            }
        }
        free(p->items);
        p->items = NULL;
    }
    p->numItems = 0;

    freeMenuItems();
}

void clearContentView() {
    clearPane(activePane());
}

static void execCommandLine(wchar_t *command) {
    SHELLEXECUTEINFO shExecInfo = {0};
    shExecInfo.cbSize = sizeof(SHELLEXECUTEINFO);
    shExecInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    shExecInfo.hwnd = hwndMain;
    shExecInfo.lpVerb = NULL;
    shExecInfo.lpFile = L"C:\\windows\\system32\\cmd.exe";
    shExecInfo.lpParameters = command;
    shExecInfo.lpDirectory = NULL;
    shExecInfo.nShow = SW_SHOW;
    shExecInfo.hInstApp = NULL;
    ShellExecuteEx(&shExecInfo);
    WaitForSingleObject(shExecInfo.hProcess, INFINITE);
    CloseHandle(shExecInfo.hProcess);
}

LRESULT CALLBACK ContentViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SETFOCUS: {
            cvSetActiveByHwnd(hwnd);
            break;
        }
        case WM_COMMAND: {
            if ((HWND)lParam == 0) {
                MENUITEMINFO item;
                item.cbSize = sizeof(MENUITEMINFO);
                item.fMask = MIIM_DATA;
                GetMenuItemInfo(hContextMenu, LOWORD(wParam), FALSE, &item);
                struct ContextMenuItem* cmItem = (struct ContextMenuItem*)item.dwItemData;

                if (cmItem->openExe) {
                    // "Open with" a specific app: launch it (non-blocking) with the file.
                    wchar_t params[MAX_PATH + 4] = {0};
                    swprintf_s(params, MAX_PATH + 4, L"\"%ls\"", cmItem->openFile);
                    ShellExecuteW(hwndMain, L"open", cmItem->openExe, params, NULL, SW_SHOW);
                }
                else if (cmItem->cmdData) {
                    wchar_t command[MAX_PATH];
                    wcscpy_s(command, MAX_PATH, L"/C ");
                    wcscat_s(command, MAX_PATH, cmItem->cmdData);
                    execCommandLine(command);
                    navigateRefresh();
                }
                else cmItem->proc();
            }
            break;
        }
        case MSG_ADD_ITEM: {
            struct Pane* p = paneFromHwnd(hwnd);
            if (p->searchData != NULL && p->searchData->active) {
                struct FileNode* node = (struct FileNode*)lParam;
                int index = p->numItems++;
                p->items = realloc(p->items, p->numItems * sizeof(struct ListItem));
                struct ListItem* item = &p->items[index];
                item->node = node;
                item->path = NULL;
                item->loaded = false;

                fillFileInfo(node, item);

                ListView_SetItemCountEx(p->hwndList, p->numItems, LVSICF_NOINVALIDATEALL);
                updateStatusbar(p);
            }
            break;
        }
        case MSG_SEARCH_DONE: {
            struct Pane* p = paneFromHwnd(hwnd);
            if (p->searchData) {
                p->searchData->active = false;
                bool canceled = p->searchData->canceled;
                free(p->searchData);
                p->searchData = NULL;
                if (canceled) {
                    refreshPane(p);
                }
                else updateStatusbar(p);
            }
            break;
        }
    }
    return OrigWndProc(hwnd, msg, wParam, lParam);
}

void updateSelectedItems() {
    struct Pane* p = activePane();
    MEMFREE(selectedItems);
    numSelectedItems = 0;

    int i = ListView_GetNextItem(p->hwndList, -1, LVNI_SELECTED);
    while (i != -1) {
        int index = numSelectedItems++;
        selectedItems = realloc(selectedItems, numSelectedItems * sizeof(struct FileNode*));
        selectedItems[index] = p->items[i].node;
        i = ListView_GetNextItem(p->hwndList, i, LVNI_SELECTED);
    }
}

static void addContextMenuItem(HMENU hMenu, int id, struct ContextMenuItem* cmItem, bool separate) {
    MENUITEMINFO item = {0};
    item.cbSize = sizeof(MENUITEMINFO);
    item.fMask = MIIM_TYPE | MIIM_DATA | MIIM_ID;
    item.fType = MFT_STRING;
    item.dwTypeData = cmItem->text;
    item.cch = wcslen(cmItem->text);
    item.wID = id;
    item.dwItemData = (ULONG_PTR)cmItem;

    InsertMenuItem(hMenu, -1, TRUE, &item);

    if (separate) {
        item.fMask = MIIM_TYPE;
        item.fType = MFT_SEPARATOR;
        InsertMenuItem(hMenu, -1, TRUE, &item);
    }
}

static void createContextMenuFromRegistry(int* id) {
    HKEY hkeyContextMenu, hkeyItem;
    if (RegOpenKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\ContextMenu", &hkeyContextMenu) != ERROR_SUCCESS) return;

    WCHAR itemName[30] = {0};
    WCHAR subitemName[100] = {0};
    WCHAR itemValue[MAX_PATH];
    DWORD i, j, itemNameLen, itemValueLen;

    i = 0;
    while (i < 10) {
        itemNameLen = 30;
        if (RegEnumKey(hkeyContextMenu, i++, itemName, itemNameLen) != ERROR_SUCCESS) break;
        if (RegOpenKey(hkeyContextMenu, itemName, &hkeyItem) == ERROR_SUCCESS) {
            MENUITEMINFO item = {0};
            item.cbSize = sizeof(MENUITEMINFO);
            item.fMask = MIIM_TYPE | MIIM_ID | MIIM_SUBMENU;
            item.fType = MFT_STRING;
            item.dwTypeData = itemName;
            item.cch = itemNameLen;
            item.wID = ++(*id);

            HMENU hSubmenu = CreatePopupMenu();

            j = 0;
            while (j < 10) {
                itemNameLen = 100;
                itemValueLen = MAX_PATH;
                if (RegEnumValue(hkeyItem, j++, subitemName, &itemNameLen, NULL, NULL, (LPBYTE)itemValue, &itemValueLen) != ERROR_SUCCESS) break;

                struct ContextMenuItem* cmItem = addMenuItemSlot();
                cmItem->text = subitemName;
                cmItem->proc = NULL;

                wchar_t *cmdData = malloc(1024);
                wcscpy_s(cmdData, MAX_PATH, itemValue);

                wchar_t path[MAX_PATH] = {0};
                getFileNodePath(selectedItems[0], path);
                cmdData = strReplace(cmdData, L"%FILE%", path, true);

                wchar_t basename[80] = {0};
                getBasenameFromPath(path, basename, true);
                cmdData = strReplace(cmdData, L"%BASENAME%", basename, true);

                getFileNodePath(selectedItems[0]->parent, path);
                cmdData = strReplace(cmdData, L"%DIR%", path, true);

                cmItem->cmdData = cmdData;
                addContextMenuItem(hSubmenu, (*id)++, cmItem, false);
            }

            item.hSubMenu = hSubmenu;

            InsertMenuItem(hContextMenu, -1, TRUE, &item);

            item.fMask = MIIM_TYPE;
            item.fType = MFT_SEPARATOR;
            InsertMenuItem(hContextMenu, -1, TRUE, &item);

            RegCloseKey(hkeyItem);
        }
    }

    RegCloseKey(hkeyContextMenu);
}

static void createCDDriveContextMenu(int* id) {
    HMENU hSubmenu = CreatePopupMenu();

    wchar_t currentISOPath[MAX_PATH] = {0};
    int currentISOPathLen = MAX_PATH;
    HKEY hkey;
    if (RegOpenKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\CurrentISOPath", &hkey) == ERROR_SUCCESS) {
        RegQueryValue(hkey, NULL, currentISOPath, (PLONG)&currentISOPathLen);
        RegCloseKey(hkey);
    }

    wchar_t itemText[64] = {0};
    swprintf_s(itemText, MAX_PATH, L"%ls <%ls>", lc_str.load_iso_image, currentISOPathLen != MAX_PATH ? currentISOPath : lc_str.no_media);
    cmiLoadISOImage.text = itemText;
    addContextMenuItem(hSubmenu, (*id)++, &cmiLoadISOImage, false);
    cmiLoadISOImage.text = NULL;

    addContextMenuItem(hSubmenu, (*id)++, &cmiUnloadISOImage, false);

    MENUITEMINFO item = {0};
    item.cbSize = sizeof(MENUITEMINFO);
    item.fMask = MIIM_TYPE | MIIM_ID | MIIM_SUBMENU;
    item.fType = MFT_STRING;
    swprintf_s(itemText, 64, L"%ls [X:]", lc_str.cd_drive);
    item.dwTypeData = itemText;
    item.cch = wcslen(itemText);
    item.wID = ++(*id);

    item.hSubMenu = hSubmenu;
    InsertMenuItem(hContextMenu, -1, TRUE, &item);

    item.fMask = MIIM_TYPE;
    item.fType = MFT_SEPARATOR;
    InsertMenuItem(hContextMenu, -1, TRUE, &item);
}

// Pull the executable path out of a registered "shell\open\command" value, e.g.
//   "C:\Program Files\App\app.exe" "%1"   ->   C:\Program Files\App\app.exe
static void extractExeFromCommand(const wchar_t* cmd, wchar_t* exeOut) {
    exeOut[0] = L'\0';
    const wchar_t* p = cmd;
    while (*p == L' ') p++;

    const wchar_t* start;
    const wchar_t* end;
    if (*p == L'"') {
        start = ++p;
        end = wcschr(p, L'"');
        if (!end) return;
    }
    else {
        start = p;
        end = wcschr(p, L' ');
        if (!end) end = p + wcslen(p);
    }

    int n = (int)(end - start);
    if (n <= 0 || n >= MAX_PATH) return;
    wcsncpy(exeOut, start, n);
    exeOut[n] = L'\0';
}

// "Open with" submenu: best-effort list of registered applications (HKCR\Applications)
// plus a "Choose another program..." entry that opens Wine's Open-With dialog. Robust when
// the registry list is sparse (still offers the dialog).
static void createOpenWithMenu(int* id) {
    wchar_t filePath[MAX_PATH] = {0};
    getFileNodePath(selectedItems[0], filePath);

    HMENU hSubmenu = CreatePopupMenu();
    int count = 0;

    HKEY hApps;
    if (RegOpenKeyW(HKEY_CLASSES_ROOT, L"Applications", &hApps) == ERROR_SUCCESS) {
        wchar_t appName[128];
        DWORD i = 0, len;
        while (count < 12) {
            len = 128;
            if (RegEnumKeyExW(hApps, i++, appName, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;

            wchar_t cmdKey[300] = {0};
            swprintf_s(cmdKey, 300, L"Applications\\%ls\\shell\\open\\command", appName);
            HKEY hCmd;
            if (RegOpenKeyW(HKEY_CLASSES_ROOT, cmdKey, &hCmd) != ERROR_SUCCESS) continue;

            wchar_t cmdVal[MAX_PATH] = {0};
            DWORD cl = sizeof(cmdVal);
            wchar_t exe[MAX_PATH] = {0};
            if (RegQueryValueExW(hCmd, NULL, NULL, NULL, (LPBYTE)cmdVal, &cl) == ERROR_SUCCESS) {
                extractExeFromCommand(cmdVal, exe);
            }
            RegCloseKey(hCmd);
            if (!exe[0] || !isPathExists(exe)) continue;

            struct ContextMenuItem* cmItem = addMenuItemSlot();
            cmItem->text = wcsdup(appName);
            cmItem->proc = NULL;
            cmItem->cmdData = NULL;
            cmItem->openExe = wcsdup(exe);
            cmItem->openFile = wcsdup(filePath);

            addContextMenuItem(hSubmenu, (*id)++, cmItem, false);
            count++;
        }
        RegCloseKey(hApps);
    }

    if (count > 0) {
        MENUITEMINFO sep = {0};
        sep.cbSize = sizeof(MENUITEMINFO);
        sep.fMask = MIIM_TYPE;
        sep.fType = MFT_SEPARATOR;
        InsertMenuItem(hSubmenu, -1, TRUE, &sep);
    }
    addContextMenuItem(hSubmenu, (*id)++, &cmiChooseProgram, false);

    MENUITEMINFO item = {0};
    item.cbSize = sizeof(MENUITEMINFO);
    item.fMask = MIIM_TYPE | MIIM_ID | MIIM_SUBMENU;
    item.fType = MFT_STRING;
    item.dwTypeData = lc_str.open_with;
    item.cch = wcslen(lc_str.open_with);
    item.wID = ++(*id);
    item.hSubMenu = hSubmenu;
    InsertMenuItem(hContextMenu, -1, TRUE, &item);
}

static void createContextMenu(enum ContextMenuType type) {
    freeMenuItems();

    HMENU hMenu = CreatePopupMenu();
    hContextMenu = hMenu;

    int id = 0;

    if (type == MENU_SINGLE || type == MENU_MULTIPLE) {
        if (type == MENU_SINGLE) {
            if (selectedItems[0]->type == TYPE_FILE) {
                addContextMenuItem(hMenu, id++, &cmiOpen, false);
                addContextMenuItem(hMenu, id++, &cmiOpenAsAdmin, false);
                createOpenWithMenu(&id);
                addContextMenuItem(hMenu, id++, &cmiEdit, true);
                createCDDriveContextMenu(&id);
                createContextMenuFromRegistry(&id);
            }
            else addContextMenuItem(hMenu, id++, &cmiOpen, true);
        }
        addContextMenuItem(hMenu, id++, &cmiCut, false);
        addContextMenuItem(hMenu, id++, &cmiCopy, true);
        addContextMenuItem(hMenu, id++, &cmiCreateShortcut, false);
        addContextMenuItem(hMenu, id++, &cmiDelete, false);

        if (type == MENU_SINGLE) addContextMenuItem(hMenu, id++, &cmiRename, false);
    }
    else {
        addContextMenuItem(hMenu, id++, &cmiPaste, false);
        addContextMenuItem(hMenu, id++, &cmiPasteShortcut, true);
        createCDDriveContextMenu(&id);
        addContextMenuItem(hMenu, id++, &cmiNewFolder, false);
        addContextMenuItem(hMenu, id++, &cmiNewFile, false);
    }

    POINT cursor;
    GetCursorPos(&cursor);
    TrackPopupMenu(hMenu, 0, cursor.x, cursor.y, 0, activePane()->hwndList, NULL);
}

LRESULT contentViewNotify(NMHDR* nmhdr) {
    struct Pane* p = paneFromHwnd(nmhdr->hwndFrom);

    switch (nmhdr->code) {
        case NM_SETFOCUS: {
            cvSetActiveByHwnd(nmhdr->hwndFrom);
            break;
        }
        case LVN_GETDISPINFO: {
            NMLVDISPINFO* nmlvdi = (NMLVDISPINFO*)nmhdr;
            UINT mask = nmlvdi->item.mask;
            struct ListItem* item = &p->items[nmlvdi->item.iItem];

            if (!item->loaded) {
                wchar_t path[MAX_PATH] = {0};
                getFileNodePath(item->node, path);

                struct FileInfo fi = {0};
                getFileInfo(path, item->node->type, p->viewStyle == STYLE_LARGE_ICON, &fi);

                if (item->node->type == TYPE_FILE) {
                    formatFileSize(item->size, item->formattedSize);

                    SYSTEMTIME systemTime = {0};
                    FILETIME localFiletime;
                    if (FileTimeToLocalFileTime(&item->modifiedTime, &localFiletime) && FileTimeToSystemTime(&localFiletime, &systemTime)) {
                        formatModifiedDate(systemTime.wMonth, systemTime.wDay, systemTime.wYear, systemTime.wHour, systemTime.wMinute, item->formattedDate, 32);
                    }
                }

                item->icon = fi.icon;
                wcscpy_s(item->type, 80, fi.typeName);
                item->loaded = true;
            }

            if (mask & LVIF_STATE) {
                nmlvdi->item.state = 0;
            }

            if (mask & LVIF_IMAGE) {
                nmlvdi->item.iImage = item->icon;
            }

            if (mask & LVIF_TEXT) {
                switch (nmlvdi->item.iSubItem) {
                    case COLUMN_NAME_IDX:
                        nmlvdi->item.pszText = item->node->name;
                        break;
                    case COLUMN_TYPE_IDX:
                        nmlvdi->item.pszText = item->type;
                        break;
                    case COLUMN_SIZE_IDX:
                        nmlvdi->item.pszText = item->node->type == TYPE_FILE ? item->formattedSize : L"";
                        break;
                    case COLUMN_DATE_IDX:
                        nmlvdi->item.pszText = item->node->type == TYPE_FILE ? item->formattedDate : L"";
                        break;
                    case COLUMN_PATH_IDX: {
                        if (!item->path) {
                            wchar_t path[MAX_PATH] = {0};
                            getFileNodePath(item->node, path);
                            item->path = wcsdup(path);
                        }
                        nmlvdi->item.pszText = item->path;
                        break;
                    }
                }
            }
            break;
        }
        case NM_RCLICK: {
            NMITEMACTIVATE* nmia = (NMITEMACTIVATE*)nmhdr;
            cvSetActiveByHwnd(nmhdr->hwndFrom);

            if (nmia->iItem != -1 && nmia->iSubItem == 0) {
                updateSelectedItems();

                bool show = true;
                for (int i = 0; i < numSelectedItems; i++) {
                    if (!(selectedItems[i]->type == TYPE_FILE || selectedItems[i]->type == TYPE_DIR)) {
                        show = false;
                        break;
                    }
                }
                if (show) createContextMenu(numSelectedItems == 1 ? MENU_SINGLE : MENU_MULTIPLE);
            }
            else createContextMenu(MENU_EMPTY);
            break;
        }
        case NM_CLICK: {
            cvSetActiveByHwnd(nmhdr->hwndFrom);
            break;
        }
        case NM_DBLCLK: {
            NMITEMACTIVATE* nmia = (NMITEMACTIVATE*)nmhdr;
            cvSetActiveByHwnd(nmhdr->hwndFrom);
            if (nmia->iItem == -1 || nmia->iSubItem != 0) break;

            struct ListItem* item = &p->items[nmia->iItem];
            openFileNode(item->node);
            break;
        }
        case LVN_COLUMNCLICK: {
            LPNMLISTVIEW plvInfo = (LPNMLISTVIEW)nmhdr;
            cvSetActiveByHwnd(nmhdr->hwndFrom);

            if (plvInfo->iSubItem == p->sortColumnIdx) {
                p->sortAscending = !p->sortAscending;
            }
            else {
                p->sortColumnIdx = plvInfo->iSubItem;
                p->sortAscending = true;
            }

            refreshPane(p);
            break;
        }
    }

    return 0;
}

static DWORD WINAPI searchTask(void* param) {
    struct SearchData* searchData = (struct SearchData*)param;
    struct Pane* p = searchData->pane;

    const int maxStackSize = 50;
    struct FileNode* stack[maxStackSize];
    int stackSize = 0;
    stack[stackSize++] = p->currPath->children;

    wchar_t keyword[64] = {0};
    wchar_t name[64] = {0};

    strToLower(searchData->keyword, keyword);

    while (stackSize > 0 && p->numItems < 10000 && searchData->active) {
        struct FileNode* node = stack[--stackSize];
        while (node && searchData->active) {
            strToLower(node->name, name);
            if (wcsstr(name, keyword)) {
                SendMessage(p->hwndList, MSG_ADD_ITEM, 0, (LPARAM)node);
            }

            if (p->numItems >= 10000) break;

            if (node->type == TYPE_DIR && stackSize < maxStackSize) {
                buildChildNodes(node, false);
                if (node->children) stack[stackSize++] = node->children;
            }
            node = node->sibling;
        }
    }

    SendMessage(p->hwndList, MSG_SEARCH_DONE, 0, 0);
    return 0;
}

void searchFor(wchar_t* keyword) {
    if (wcslen(keyword) == 0) return;
    struct Pane* p = activePane();
    if (p->searchData != NULL && p->searchData->active) {
        p->searchData->active = false;
        return;
    }

    clearPane(p);

    LVCOLUMN column = {0};
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.cx = 250;
    column.pszText = lc_str.path;
    ListView_InsertColumn(p->hwndList, COLUMN_PATH_IDX, &column);
    UpdateWindow(p->hwndList);

    p->searchData = malloc(sizeof(struct SearchData));
    p->searchData->keyword = keyword;
    p->searchData->active = true;
    p->searchData->canceled = false;
    p->searchData->pane = p;

    CreateThread(NULL, 0, searchTask, p->searchData, 0, NULL);
}

void setViewStyle(enum ViewStyle newViewStyle) {
    struct Pane* p = activePane();
    LONG_PTR wndstyle = GetWindowLongPtr(p->hwndList, GWL_STYLE);
    wndstyle &= ~LVS_TYPEMASK;

    switch (newViewStyle) {
        case STYLE_LARGE_ICON:
            wndstyle |= LVS_ICON;
            break;
        case STYLE_SMALL_ICON:
            wndstyle |= LVS_SMALLICON;
            break;
        case STYLE_LIST:
            wndstyle |= LVS_LIST;
            break;
        case STYLE_DETAILS:
            wndstyle |= LVS_REPORT;
            break;
    }

    SetWindowLongPtr(p->hwndList, GWL_STYLE, wndstyle);

    p->viewStyle = newViewStyle;
    refreshPane(p);
}

static void createLVColumns(HWND hwndList) {
    LVCOLUMN column = {0};
    column.mask = LVCF_WIDTH | LVCF_TEXT;

    column.cx = 220;
    column.pszText = lc_str.name;
    ListView_InsertColumn(hwndList, COLUMN_NAME_IDX, &column);

    column.cx = 100;
    column.pszText = lc_str.type;
    ListView_InsertColumn(hwndList, COLUMN_TYPE_IDX, &column);

    column.cx = 60;
    column.pszText = lc_str.size;
    ListView_InsertColumn(hwndList, COLUMN_SIZE_IDX, &column);

    column.cx = 100;
    column.pszText = lc_str.date;
    ListView_InsertColumn(hwndList, COLUMN_DATE_IDX, &column);
}

static HWND createOneContentView() {
    HWND hwnd = CreateWindowEx(0, WC_LISTVIEW, NULL, WS_VISIBLE | WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_BORDER | LVS_OWNERDATA | LVS_REPORT | LVS_SHAREIMAGELISTS,
                               0, 0, 0, 0, hwndMain, (HMENU)NULL, globalHInstance, NULL);
    OrigWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)ContentViewWndProc);
    SendMessage(hwnd, WM_SETFONT, (WPARAM)getUIFont(), TRUE);
    // Modern list behaviour: full-row selection, flicker-free scrolling, tidy label tips.
    ListView_SetExtendedListViewStyle(hwnd, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowTheme(hwnd, L"Explorer", NULL);
    createLVColumns(hwnd);
    UpdateWindow(hwnd);
    return hwnd;
}

void createContentView() {
    cmiOpen.text = lc_str.open;
    cmiEdit.text = lc_str.edit;
    cmiCut.text = lc_str.cut;
    cmiCopy.text = lc_str.copy;
    cmiCreateShortcut.text = lc_str.create_shortcut;
    cmiDelete.text = lc_str.delete;
    cmiRename.text = lc_str.rename;
    cmiPaste.text = lc_str.paste;
    cmiPasteShortcut.text = lc_str.paste_shortcut;
    cmiNewFolder.text = lc_str.new_folder;
    cmiNewFile.text = lc_str.new_file;
    cmiUnloadISOImage.text = lc_str.unload_iso_image;
    cmiOpenAsAdmin.text = lc_str.open_as_admin;
    cmiChooseProgram.text = lc_str.choose_program;

    for (int i = 0; i < NUM_PANES; i++) {
        panes[i].hwndList = createOneContentView();
        panes[i].currPath = NULL;
        panes[i].items = NULL;
        panes[i].numItems = 0;
        panes[i].viewStyle = STYLE_DETAILS;
        panes[i].sortColumnIdx = COLUMN_NAME_IDX;
        panes[i].sortAscending = true;
        panes[i].searchData = NULL;
    }

    // Pane 1 starts hidden until split view is enabled.
    ShowWindow(panes[1].hwndList, SW_HIDE);
    activeIdx = 0;
}

// Give each pane its own independent path chain. Called after initFileNodes()
// (which leaves the global currPathFileNode pointing at Computer).
void cvInitPanePaths() {
    panes[0].currPath = currPathFileNode;                 // adopt the initial chain
    panes[1].currPath = copyPathChain(currPathFileNode);  // independent copy
    activeIdx = 0;
    currPathFileNode = panes[0].currPath;
}

static void cvSetActiveByHwnd(HWND h) {
    int idx = activeIdx;
    for (int i = 0; i < NUM_PANES; i++) {
        if (panes[i].hwndList == h) { idx = i; break; }
    }
    if (idx == activeIdx) return;
    if (!splitOn) return;

    // Swap the active pane's live path out to its slot, bring the new one in.
    panes[activeIdx].currPath = currPathFileNode;
    activeIdx = idx;
    currPathFileNode = panes[activeIdx].currPath;

    SetWindowText(hwndMain, currPathFileNode->name);
    updateAddrButtons();
    updateStatusbar(activePane());
    cvInvalidatePaneFrames();
}

void cvToggleSplit() {
    splitOn = !splitOn;

    if (splitOn) {
        ShowWindow(panes[1].hwndList, SW_SHOW);
        // Populate pane 1 (it was never refreshed while hidden).
        buildChildNodes(panes[1].currPath, false);
        refreshPane(&panes[1]);
    }
    else {
        // Collapsing: make pane 0 active and hide pane 1.
        if (activeIdx == 1) {
            panes[1].currPath = currPathFileNode;
            activeIdx = 0;
            currPathFileNode = panes[0].currPath;
            SetWindowText(hwndMain, currPathFileNode->name);
            updateAddrButtons();
            updateStatusbar(activePane());
        }
        ShowWindow(panes[1].hwndList, SW_HIDE);
    }

    resizeControls();
    cvInvalidatePaneFrames();
    SetFocus(panes[activeIdx].hwndList);
}

void onMenuItemUpClick() {
    navigateUp();
}

void onMenuItemOpenClick() {
    if (numSelectedItems == 1) openFileNode(selectedItems[0]);
}

void onMenuItemOpenAsAdminClick() {
    if (numSelectedItems == 1 && selectedItems[0]->type == TYPE_FILE) {
        wchar_t path[MAX_PATH] = {0};
        wchar_t parentPath[MAX_PATH] = {0};
        getFileNodePath(selectedItems[0], path);
        getFileNodePath(selectedItems[0]->parent, parentPath);
        // "runas" verb requests elevation. Wine's elevation is largely cosmetic, but
        // apps that gate on the verb / the elevated flag get what they expect.
        ShellExecuteW(hwndMain, L"runas", path, NULL, parentPath, SW_SHOW);
    }
}

void onMenuItemOpenWithClick() {
    if (numSelectedItems == 1 && selectedItems[0]->type == TYPE_FILE) {
        wchar_t path[MAX_PATH] = {0};
        getFileNodePath(selectedItems[0], path);
        // "openas" verb → Wine's "Open With" dialog (lists compatible programs + browse).
        SHELLEXECUTEINFOW sei = {0};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_INVOKEIDLIST;
        sei.hwnd = hwndMain;
        sei.lpVerb = L"openas";
        sei.lpFile = path;
        sei.nShow = SW_SHOW;
        ShellExecuteExW(&sei);
    }
}

void onMenuItemEditClick() {
    if (numSelectedItems == 1 && selectedItems[0]->type == TYPE_FILE) {
        static const wchar_t editorPath[] = L"C:\\windows\\notepad.exe";

        wchar_t path[MAX_PATH] = {0};
        wchar_t parameters[MAX_PATH] = {0};
        getFileNodePath(selectedItems[0], path);
        swprintf_s(parameters, MAX_PATH, L"\"%ls\"", path);
        getFileNodePath(selectedItems[0]->parent, path);
        ShellExecute(hwndMain, L"open", editorPath, parameters, path, SW_SHOW);
    }
}

void onMenuItemCutClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) cutFiles(selectedItems, numSelectedItems);
}

void onMenuItemCopyClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) copyFiles(selectedItems, numSelectedItems);
}

void onMenuItemCreateShortcutClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) createDesktopShortcuts(selectedItems, numSelectedItems);
}

void onMenuItemDeleteClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) deleteFiles(selectedItems, numSelectedItems);
}

void onMenuItemRenameClick() {
    if (numSelectedItems == 1) {
        wchar_t* result = InputDialog(lc_str.rename, lc_str.enter_new_name, selectedItems[0]->name, true);
        if (result) {
            wchar_t newFilename[MAX_PATH] = {0};
            getFileNodePath(selectedItems[0]->parent, newFilename);
            wcscat_s(newFilename, MAX_PATH, L"\\");
            wcscat_s(newFilename, MAX_PATH, result);
            free(result);

            wchar_t oldFilename[MAX_PATH] = {0};
            getFileNodePath(selectedItems[0], oldFilename);
            MoveFileW(oldFilename, newFilename);
            navigateRefresh();
        }
    }
}

void onMenuItemPasteClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;
    pasteFiles(path);
}

void onMenuItemPasteShortcutClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;
    pasteShortcuts(path);
}

void onMenuItemNewFolderClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;

    wchar_t* result = InputDialog(lc_str.new_folder, lc_str.enter_folder_name, NULL, false);
    if (result) {
        wcscat_s(path, MAX_PATH, L"\\");
        wcscat_s(path, MAX_PATH, result);
        free(result);

        if (!isPathExists(path)) {
            CreateDirectory(path, NULL);
            navigateRefresh();
        }
    }
}

void onMenuItemNewFileClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;

    wchar_t* result = InputDialog(lc_str.new_file, lc_str.enter_file_name, NULL, false);
    if (result) {
        wcscat_s(path, MAX_PATH, L"\\");
        wcscat_s(path, MAX_PATH, result);
        free(result);

        if (!isPathExists(path)) {
            HANDLE handle = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            navigateRefresh();
        }
    }
}

void onMenuItemSelectAllClick() {
    HWND h = activePane()->hwndList;
    ListView_SetItemState(h, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(h, -1, LVIS_SELECTED, LVIS_SELECTED);
    SetFocus(h);
}

static void onMenuItemLoadISOImageClick() {
    if (numSelectedItems != 1) {
        MessageBox(NULL, lc_str.msg_invalid_iso_image_file, lc_str.alert, MB_OK);
        return;
    }

    wchar_t currentISOPath[MAX_PATH] = {0};
    HKEY hkey;
    getFileNodePath(selectedItems[0], currentISOPath);

    if (!isPathExists(currentISOPath) || !(hasFileExtension(currentISOPath, L"iso") ||
                                           hasFileExtension(currentISOPath, L"bin") ||
                                           hasFileExtension(currentISOPath, L"cue"))) {
        MessageBox(NULL, lc_str.msg_invalid_iso_image_file, lc_str.alert, MB_OK);
        return;
    }

    if (RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\CurrentISOPath", &hkey) == ERROR_SUCCESS) {
        RegSetValue(hkey, NULL, REG_SZ, currentISOPath, (wcslen(currentISOPath) + 1) * sizeof(wchar_t));
        RegCloseKey(hkey);
    }

    clearDirectory(L"X:");
    extractFilesFromISOImage(currentISOPath, L"X:\\");
}

static void onMenuItemUnloadISOImageClick() {
    clearDirectory(L"X:");
    RegDeleteKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\CurrentISOPath");
    navigateRefresh();
}

static int compareType(const void* a, const void* b) {
    struct ListItem* ia = (struct ListItem*)a;
    struct ListItem* ib = (struct ListItem*)b;
    return g_sortPane->sortAscending ? ia->node->type - ib->node->type : ib->node->type - ia->node->type;
}

static int compareName(const void* a, const void* b) {
    struct ListItem* ia = (struct ListItem*)a;
    struct ListItem* ib = (struct ListItem*)b;
    int res = compareType(a, b);
    if (res == 0) res = g_sortPane->sortAscending ? wcscoll(ia->node->name, ib->node->name) : wcscoll(ib->node->name, ia->node->name);
    return res;
}

static int compareSize(const void* a, const void* b) {
    struct ListItem* ia = (struct ListItem*)a;
    struct ListItem* ib = (struct ListItem*)b;
    int res = compareType(a, b);
    if (res == 0) res = g_sortPane->sortAscending ? ia->size - ib->size : ib->size - ia->size;
    return res;
}

static int compareDate(const void* a, const void* b) {
    struct ListItem* ia = (struct ListItem*)a;
    struct ListItem* ib = (struct ListItem*)b;
    int res = compareType(a, b);
    if (res == 0) res = g_sortPane->sortAscending ? CompareFileTime(&ia->modifiedTime, &ib->modifiedTime) : CompareFileTime(&ib->modifiedTime, &ia->modifiedTime);
    return res;
}

static void sortItems(struct Pane* p) {
    g_sortPane = p;
    switch (p->sortColumnIdx) {
        case COLUMN_NAME_IDX:
            qsort(p->items, p->numItems, sizeof(struct ListItem), compareName);
            break;
        case COLUMN_TYPE_IDX:
            qsort(p->items, p->numItems, sizeof(struct ListItem), compareType);
            break;
        case COLUMN_SIZE_IDX:
            qsort(p->items, p->numItems, sizeof(struct ListItem), compareSize);
            break;
        case COLUMN_DATE_IDX:
            qsort(p->items, p->numItems, sizeof(struct ListItem), compareDate);
            break;
    }
}

static void refreshPane(struct Pane* p) {
    if (p->searchData != NULL && p->searchData->active) {
        p->searchData->active = false;
        p->searchData->canceled = true;
        return;
    }

    clearPane(p);
    UpdateWindow(p->hwndList);

    struct FileNode* child = p->currPath->children;

    p->numItems = getChildNodeCount(p->currPath);
    p->items = calloc(p->numItems, sizeof(struct ListItem));
    int index = 0;

    while (child) {
        struct ListItem* item = &p->items[index++];
        item->node = child;
        item->loaded = false;

        fillFileInfo(child, item);

        child = child->sibling;
    }

    HIMAGELIST himlBig, himlSmall;
    Shell_GetImageLists(&himlBig, &himlSmall);

    if (p->viewStyle == STYLE_LARGE_ICON) {
        ListView_SetImageList(p->hwndList, himlBig, LVSIL_NORMAL);
    }
    else ListView_SetImageList(p->hwndList, himlSmall, LVSIL_SMALL);

    if (p->sortColumnIdx != -1) sortItems(p);
    ListView_SetItemCountEx(p->hwndList, p->numItems, 0);

    updateStatusbar(p);
}

void refreshContentView() {
    // Keep the active pane's stored path in sync with the global cursor, then refresh it.
    activePane()->currPath = currPathFileNode;
    refreshPane(activePane());
}
