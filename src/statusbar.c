#include "main.h"

extern HINSTANCE globalHInstance;
extern HWND hwndMain;

HWND hwndStatusbar = NULL;

// Wine renders the status bar with a light background; owner-draw it dark to match.
static WNDPROC OrigStatusProc = NULL;
static wchar_t statusText[160] = {0};

static LRESULT CALLBACK StatusWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(45, 45, 45));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        HGDIOBJ oldFont = SelectObject(hdc, getUIFont());
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(220, 220, 220));
        RECT tr = rc;
        tr.left += 8;
        DrawText(hdc, statusText, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, oldFont);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProc(OrigStatusProc, hwnd, msg, wParam, lParam);
}

void setStatusbarText(wchar_t* text) {
    wcscpy_s(statusText, 160, text ? text : L"");
    InvalidateRect(hwndStatusbar, NULL, TRUE);
}

void createStatusbar() {
    hwndStatusbar = CreateWindowEx(0, STATUSCLASSNAME, NULL, WS_VISIBLE | WS_CHILD | WS_CLIPSIBLINGS,
                                   0, 0, 0, 0, hwndMain, (HMENU)NULL, globalHInstance, NULL);
    SendMessage(hwndStatusbar, WM_SETFONT, (WPARAM)getUIFont(), TRUE);
    OrigStatusProc = (WNDPROC)SetWindowLongPtr(hwndStatusbar, GWLP_WNDPROC, (LONG_PTR)StatusWndProc);
    UpdateWindow(hwndStatusbar);
}
