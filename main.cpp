#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <CommCtrl.h>
#include <Uxtheme.h>
#include <shellapi.h>
#include <tchar.h>
#include <time.h>
#include <vector>
#include "resource.h"

#include "curl_http.h"
#include "tinyxml2.h"

#pragma comment(linker, \
  "\"/manifestdependency:type='Win32' "\
  "name='Microsoft.Windows.Common-Controls' "\
  "version='6.0.0.0' "\
  "processorArchitecture='*' "\
  "publicKeyToken='6595b64144ccf1df' "\
  "language='*'\"")
#pragma comment(lib, "ComCtl32.lib")
#pragma comment(lib, "Uxtheme.lib")

#define APPWM_ICONNOTIFY (WM_APP + 1)

enum class ProjectActivity
{
    Building, Sleeping, Unknown
};

enum class BuildState
{
    Success, Failure, Unknown
};

struct Project
{
    std::string name, webUrl;
    BuildState lastBuildStatus;
    ProjectActivity activity;
    int lastBuildLabel;
};

enum class BuildIcons {
    Gray, GrayBuilding, Green, GreenBuilding, Red, RedBuilding, Count
};
const char* gBuildIconNames[(int)BuildIcons::Count] = {
    "Unknown", "Building", "All Successful", "Building", "Build Failed", "Building"
};

HINSTANCE ghInst;
HWND gMainWindow;
HICON gIconSettings;
HICON gBuildIcons[(int)BuildIcons::Count];
BuildIcons gBestIcon;
char gServerPath[512];
char gStatusText[1024];
time_t gNextPollTime = 0, gLastPollTime = 0;
HttpRequest* gCurrentPoll = nullptr;
std::vector<Project> gProjects;
bool gWindowVisible = true;

template<size_t count, typename... Args>
static void sprintf_safe(char(&output)[count], const char* format, Args... args)
{
    snprintf(output, count, format, args...);
}

INT_PTR CALLBACK SettingsWindowDialog(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
        SetWindowText(GetDlgItem(hDlg, IDC_SERVERPATH), gServerPath);
        SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)gIconSettings);
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_OK:
            GetWindowText(GetDlgItem(hDlg, IDC_SERVERPATH), gServerPath, sizeof(gServerPath));
            gNextPollTime = 0;
            EndDialog(hDlg, 0);
            break;
        case IDC_CANCEL:
            EndDialog(hDlg, 0);
            break;
        }
        break;
    case WM_CLOSE:
        EndDialog(hDlg, 0);
        break;
    }
    
    return FALSE;
}

static Project* GetSelectedProject()
{
    HWND hLst = GetDlgItem(gMainWindow, IDC_BUILDS);
    int selectedIdx = SendMessage(hLst, LVM_GETNEXTITEM, -1, LVNI_FOCUSED);
    if (selectedIdx == -1)
        return nullptr;

    LV_ITEM item = {};
    item.iItem = selectedIdx;
    item.mask = LVIF_PARAM;
    ListView_GetItem(hLst, &item);
    return (Project*)item.lParam;
}

INT_PTR CALLBACK MainWindowDialog(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_VIEWJENKINS:
        {
            Project* proj = GetSelectedProject();
            if (proj) {
                ShellExecute(gMainWindow, 0, proj->webUrl.c_str(), 0, 0, SW_SHOW);
            }
        }
            break;
        case ID_RUNBUILD:
            //TODO LOL
            break;
        case ID_CONTEXT_OPENBUTLER:
            ShowWindow(gMainWindow, SW_SHOW);
            gWindowVisible = true;
            break;
        case ID_CONTEXT_EXIT:
            PostQuitMessage(0);
            break;
        case IDC_SETTINGS:
            DialogBox(ghInst, MAKEINTRESOURCE(IDD_SETTINGS), hDlg, SettingsWindowDialog);
            break;
        case IDC_REFRESH:
            gNextPollTime = 0;
            break;
        }
        break;
    case WM_NOTIFY:
        switch (LOWORD(wParam))
        {
        case IDC_BUILDS:
            int clickEvent = ((LPNMHDR)lParam)->code;
            if (clickEvent == NM_RCLICK || clickEvent == NM_DBLCLK) {
                Project* proj = GetSelectedProject();
                if (proj) {
                    if (clickEvent == NM_DBLCLK) {
                        ShellExecute(gMainWindow, 0, proj->webUrl.c_str(), 0, 0, SW_SHOW);
                    }
                    else if (clickEvent == NM_RCLICK) {
                        HMENU hMenu = LoadMenu(ghInst, MAKEINTRESOURCE(IDR_BUILD_CONTEXT_MENU));
                        HMENU hPopupMenu = GetSubMenu(hMenu, 0);
                        POINT pt;
                        SetMenuDefaultItem(hPopupMenu, 0, TRUE);
                        GetCursorPos(&pt);
                        SetForegroundWindow(gMainWindow);
                        TrackPopupMenu(hPopupMenu, TPM_LEFTALIGN, pt.x, pt.y, 0, gMainWindow, nullptr);
                        SetForegroundWindow(gMainWindow);
                        DestroyMenu(hPopupMenu);
                        DestroyMenu(hMenu);
                    }
                }
            }
            break;
        }
        break;
    case APPWM_ICONNOTIFY:
        if (lParam == WM_LBUTTONUP) {
            ShowWindow(gMainWindow, gWindowVisible ? SW_HIDE : SW_SHOW);
            gWindowVisible = !gWindowVisible;
        }
        else if (lParam == WM_RBUTTONUP) {
            HMENU hMenu = LoadMenu(ghInst, MAKEINTRESOURCE(IDR_TRAY_CONTEXT_MENU));
            HMENU hPopupMenu = GetSubMenu(hMenu, 0);
            POINT pt;
            SetMenuDefaultItem(hPopupMenu, 0, TRUE);
            GetCursorPos(&pt);
            SetForegroundWindow(gMainWindow);
            TrackPopupMenu(hPopupMenu, TPM_LEFTALIGN, pt.x, pt.y, 0, gMainWindow, nullptr);
            SetForegroundWindow(gMainWindow);
            DestroyMenu(hPopupMenu);
            DestroyMenu(hMenu);
        }
        break;
    case WM_CLOSE:
        ShowWindow(gMainWindow, SW_HIDE);
        gWindowVisible = false;
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return TRUE;
    }

    return FALSE;
}

static bool ParseProjects(const char* data, size_t len, std::vector<Project>& projects)
{
    projects.clear();
    tinyxml2::XMLDocument doc;
    doc.Parse(data, len);
    tinyxml2::XMLElement* proj_xml = doc.FirstChildElement("Projects");
    if (!proj_xml) return false;

    for (tinyxml2::XMLElement* proj = proj_xml->FirstChildElement("Project"); proj; proj = proj->NextSiblingElement("Project")) {
        Project project;
        project.name = std::string(proj->Attribute("name"));
        project.webUrl = std::string(proj->Attribute("webUrl"));
        project.lastBuildLabel = proj->IntAttribute("lastBuildLabel");
        const char* activity = proj->Attribute("activity");
        if (!activity) activity = "";
        if (strcmp(activity, "Sleeping") == 0)
            project.activity = ProjectActivity::Sleeping;
        else if (strcmp(activity, "Building") == 0)
            project.activity = ProjectActivity::Building;
        else
            project.activity = ProjectActivity::Unknown;

        const char* lastBuildStatus = proj->Attribute("lastBuildStatus");
        if (!lastBuildStatus) lastBuildStatus = "";
        if (strcmp(lastBuildStatus, "Success") == 0)
            project.lastBuildStatus = BuildState::Success;
        else if (strcmp(lastBuildStatus, "Failure") == 0)
            project.lastBuildStatus = BuildState::Failure;
        else
            project.lastBuildStatus = BuildState::Unknown;
        projects.push_back(project);
    }
    return true;
}

static void SetProjects(const std::vector<Project>& projects)
{
    HWND hLst = GetDlgItem(gMainWindow, IDC_BUILDS);
    ListView_DeleteAllItems(hLst);

    LV_ITEM item{};
    item.cchTextMax = 40;
    item.mask = LVIF_IMAGE | LVIF_TEXT | LVIF_PARAM;
    BuildIcons bestIcon = BuildIcons::Gray;
    for (const Project& proj : projects) {
        item.pszText = (char*)proj.name.data();
        item.lParam = (LPARAM)&proj;
        if (proj.lastBuildStatus == BuildState::Failure)
            item.iImage = proj.activity == ProjectActivity::Building ? (int)BuildIcons::RedBuilding : (int)BuildIcons::Red;
        else if (proj.lastBuildStatus == BuildState::Success)
            item.iImage = proj.activity == ProjectActivity::Building ? (int)BuildIcons::GreenBuilding : (int)BuildIcons::Green;
        else
            item.iImage = proj.activity == ProjectActivity::Building ? (int)BuildIcons::GrayBuilding : (int)BuildIcons::Gray;

        if (item.iImage > (int)bestIcon)
            bestIcon = (BuildIcons)item.iImage;

        ListView_InsertItem(hLst, &item);
    }

    gBestIcon = bestIcon;
}

static void SetStatus(const char* msg)
{
    NOTIFYICONDATA nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = gMainWindow;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = APPWM_ICONNOTIFY;
    nid.hIcon = gBuildIcons[(int)gBestIcon];

    sprintf_safe(nid.szTip, "Butler: %s\n%s", gBuildIconNames[(int)gBestIcon], msg);

    Shell_NotifyIcon(NIM_MODIFY, &nid);
    SetClassLong(gMainWindow, GCL_HICON, (LONG)gBestIcon);
    SendMessage(gMainWindow, WM_SETICON, ICON_BIG, (LPARAM)gBestIcon);
    SendMessage(gMainWindow, WM_SETICON, ICON_SMALL, (LPARAM)gBestIcon);

    SetDlgItemText(gMainWindow, IDC_STATUS, msg);
    RedrawWindow(gMainWindow, nullptr, nullptr, RDW_INVALIDATE);
}

int WINAPI _tWinMain(HINSTANCE hInst, HINSTANCE h0, LPTSTR lpCmdLine, int nCmdShow)
{
    ghInst = hInst;

    //Create and show the window
    InitCommonControls();
    gMainWindow = CreateDialogParam(hInst, MAKEINTRESOURCE(IDD_MAINWINDOW), 0, MainWindowDialog, 0);
    gIconSettings = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SETTINGS));
    SendMessage(GetDlgItem(gMainWindow, IDC_SETTINGS), BM_SETIMAGE, (WPARAM)IMAGE_ICON, (LPARAM)gIconSettings);
    SendMessage(GetDlgItem(gMainWindow, IDC_REFRESH), BM_SETIMAGE, (WPARAM)IMAGE_ICON, (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_REFRESH)));

    //Create the build icons image list
    HIMAGELIST hImgList = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ILC_COLOR32, 3, 1);
    int iconIDs[(int)BuildIcons::Count] = {
        IDI_GRAY, IDI_GRAY_BUILDING, IDI_GREEN, IDI_GREEN_BUILDING, IDI_RED, IDI_RED_BUILDING
    };
    for (int i = 0; i < (int)BuildIcons::Count; ++i) {
        gBuildIcons[i] = LoadIcon(hInst, MAKEINTRESOURCE(iconIDs[i]));
        ImageList_AddIcon(hImgList, gBuildIcons[i]);
    }

    //Set the gray icon onto the window whilst we ask for build state
    SendMessage(gMainWindow, WM_SETICON, ICON_BIG, (LPARAM)gBuildIcons[(int)BuildIcons::Gray]);
    SendMessage(gMainWindow, WM_SETICON, ICON_SMALL, (LPARAM)gBuildIcons[(int)BuildIcons::Gray]);
    SetClassLong(gMainWindow, GCL_HICON, (LONG)gBuildIcons[(int)BuildIcons::Gray]);

    //Setup builds list view styles etc
    {
        HWND hLst = GetDlgItem(gMainWindow, IDC_BUILDS);
        ListView_SetImageList(hLst, hImgList, LVSIL_SMALL);

        LV_COLUMN col = { 0 };
        col.fmt = LVCFMT_IMAGE | LVCFMT_FILL | LVCFMT_RIGHT;
        col.mask = LVCF_FMT | LVCF_TEXT | LVCF_WIDTH;
        col.cx = 258;
        col.pszText = "Name";
        ListView_InsertColumn(hLst, 0, &col);

        ListView_SetView(hLst, LV_VIEW_DETAILS);
        SetWindowTheme(hLst, L"Explorer", NULL);
        SetWindowLong(hLst, GWL_STYLE, GetWindowLong(hLst, GWL_STYLE) | LVS_NOCOLUMNHEADER);
        SendMessage(hLst, WM_CHANGEUISTATE, MAKELONG(UIS_SET, UISF_HIDEFOCUS), 0);
        ListView_SetExtendedListViewStyle(hLst, LVS_EX_DOUBLEBUFFER);
    }

    //Setup notification tray
    {
        NOTIFYICONDATA nid = {};
        nid.cbSize = sizeof(NOTIFYICONDATA);
        nid.hWnd = gMainWindow;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = APPWM_ICONNOTIFY;
        nid.hIcon = gBuildIcons[(int)BuildIcons::Gray];
        sprintf_safe(nid.szTip, "Butler: Fetching");
        Shell_NotifyIcon(NIM_ADD, &nid);
    }

    sprintf_safe(gServerPath, "https://builds.apache.org/cc.xml");

    MSG msg;
    while (GetMessage(&msg, 0, 0, 0)) {
        //Update messages
        if (gCurrentPoll) {
            HttpStatus t = http_process(gCurrentPoll);
            if (t != HttpStatus::Pending) {
                gLastPollTime = time(nullptr);
                gNextPollTime = gLastPollTime + 60;
                if (t == HttpStatus::Completed) {
                    if (ParseProjects(gCurrentPoll->response_data, gCurrentPoll->response_data_len, gProjects)) {
                        SetProjects(gProjects);
                        tm* timeinfo = localtime(&gLastPollTime);
                        strftime(gStatusText, sizeof(gStatusText), "Refreshed at %H:%M:%S", timeinfo);
                        SetStatus(gStatusText);
                    }
                    else
                        SetStatus("Failed to parse response!");
                }
                else {
                    sprintf_safe(gStatusText, "Failed %d: %s", gCurrentPoll->status_code, (const char*)gCurrentPoll->response_data);
                    SetStatus(gStatusText);
                }
                http_release(gCurrentPoll);
                gCurrentPoll = nullptr;
            }
        }
        else {
            time_t currentTime = time(nullptr);
            if (currentTime >= gNextPollTime) {
                sprintf_safe(gStatusText, "Fetching %s", gServerPath);
                SetStatus(gStatusText);
                gCurrentPoll = http_get(gServerPath);
            }
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    {
        NOTIFYICONDATA nid = {};
        nid.cbSize = sizeof(NOTIFYICONDATA);
        nid.hWnd = gMainWindow;
        nid.uID = 1;
        Shell_NotifyIcon(NIM_DELETE, &nid);
    }

    return 0;
}
