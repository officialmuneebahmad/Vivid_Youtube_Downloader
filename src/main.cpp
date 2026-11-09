#define _WIN32_WINNT 0x0600
#define _WIN32_IE    0x0500

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <atomic>
#include <vector>
#include <sstream>
#include <iostream>

#include "GUITheme.h"
#include "Downloader.h"
#include "History.h"

// Link common controls v6 manifest inclusion (MSVC only)
#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "msimg32.lib")
#endif

// Control IDs
#define IDC_URL_EDIT         1001
#define IDC_PASTE_BTN        1002
#define IDC_QUALITY_COMBO    1003
#define IDC_MP3_CHK          1004
#define IDC_PLAYLIST_CHK     1005
#define IDC_DOWNLOAD_BTN     1006
#define IDC_CANCEL_BTN       1007
#define IDC_PROGRESS_BAR     1008
#define IDC_HISTORY_LIST     1009
#define IDC_CLEAR_HIST_BTN   1010
#define IDC_THEME_CHK        1011
#define IDC_SHOW_DOWNLOADS_BTN 1012
#define IDC_THEME_LBL        1013
#define IDC_MP3_LBL          1014
#define IDC_PLAYLIST_LBL     1015
#define IDC_CHECK_UPDATES_BTN 1016

// Global UI Handles
HWND g_hWnd = NULL;
HWND g_hUrlEdit = NULL;
HWND g_hPasteBtn = NULL;
HWND g_hCombo = NULL;
HWND g_hMp3Chk = NULL;
HWND g_hPlaylistChk = NULL;
HWND g_hDownloadBtn = NULL;
HWND g_hCancelBtn = NULL;
HWND g_hStatusLabel = NULL;
HWND g_hProgressBar = NULL;
HWND g_hHistoryList = NULL;
HWND g_hClearHistBtn = NULL;
HWND g_hCheckUpdatesBtn = NULL;
HWND g_hThemeChk = NULL;
HWND g_hShowDownloadsBtn = NULL;

HFONT g_hFont = NULL;
Theme g_Theme = {};  // zero-init to avoid null HBRUSH crash before first theme is created
std::atomic<bool> g_CancelDownload(false);
bool g_Downloading = false;
std::string g_CurrentUrl = "";
int g_CurrentQuality = 0;
bool g_CurrentMp3 = false;
bool g_CurrentPlaylist = false;

// Helper to set Immersive Dark Mode header via dynamic DLL loading
typedef HRESULT (WINAPI *DwmSetWindowAttributePtr)(HWND, DWORD, LPCVOID, DWORD);

void SetWindowDarkHeader(HWND hWnd, BOOL dark) {
    HMODULE hDwm = LoadLibraryA("dwmapi.dll");
    if (hDwm) {
        DwmSetWindowAttributePtr pSetAttribute = (DwmSetWindowAttributePtr)GetProcAddress(hDwm, "DwmSetWindowAttribute");
        if (pSetAttribute) {
            BOOL bDark = dark;
            pSetAttribute(hWnd, 20, &bDark, sizeof(bDark));
            pSetAttribute(hWnd, 19, &bDark, sizeof(bDark));
        }
        FreeLibrary(hDwm);
    }
}

// Read from clipboard
void PasteFromClipboard(HWND hUrlEdit) {
    if (!OpenClipboard(NULL)) return;
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (hData != NULL) {
        char* pszText = (char*)GlobalLock(hData);
        if (pszText != NULL) {
            SetWindowTextA(hUrlEdit, pszText);
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
}

// Refresh History ListBox
void RefreshHistoryList(HWND hListBox) {
    (void)SendMessage(hListBox, LB_RESETCONTENT, 0, 0);
    std::vector<HistoryEntry> history = LoadHistory();
    for (const auto& entry : history) {
        std::stringstream ss;
        ss << entry.dateTime << "  |  " << entry.title << "  [" << entry.quality << "]  -  " << entry.status;
        std::string displayStr = ss.str();
        SendMessageA(hListBox, LB_ADDSTRING, 0, (LPARAM)displayStr.c_str());
    }
}

// Quality Name helper
static const std::string& GetQualityLabel(int index) {
    static const std::string labels[] = {
        "Best Quality",   // 0
        "1080p Video",    // 1
        "720p Video",     // 2
        "480p Video",     // 3
        "MP3 (320k)",     // 4
        "MP3 (192k)",     // 5
        "M4A Audio"       // 6
    };
    static const std::string fallback = "Best Quality";
    if (index >= 0 && index <= 6) return labels[index];
    return fallback;
}

// Redraw child callback for theme changes
static BOOL CALLBACK RedrawChildProc(HWND hChild, LPARAM lParam) {
    (void)lParam; // Suppress unused parameter warning
    InvalidateRect(hChild, NULL, TRUE);
    UpdateWindow(hChild);
    return TRUE;
}

// Global theme helper
void ToggleTheme(HWND hWnd, BOOL isDark) {
    FreeTheme(g_Theme);
    g_Theme = isDark ? CreateDarkTheme() : CreateLightTheme();
    SetWindowDarkHeader(hWnd, isDark);

    // Redraw window and child items
    InvalidateRect(hWnd, NULL, TRUE);
    EnumChildWindows(hWnd, RedrawChildProc, 0);
}

// Background worker thread for updating/verifying yt-dlp and ffmpeg
DWORD WINAPI UpdateCheckThreadProc(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;
    std::string report = CheckAndUpdateEngines(hWnd);
    std::string* pReport = new std::string(report);
    PostMessage(hWnd, WM_UPDATE_CHECK_DONE, 0, (LPARAM)pReport);
    return 0;
}

// Background worker thread procedure using native Windows thread API
DWORD WINAPI DownloadThreadProc(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;

    // 1. Dependency checks
    if (!SetupDependencies(hWnd, g_CancelDownload)) {
        std::string* pErr;
        if (g_CancelDownload) {
            pErr = new std::string("Dependency setup cancelled");
        } else {
            pErr = new std::string("Failed to setup yt-dlp/ffmpeg dependencies.");
        }
        PostMessage(hWnd, WM_DOWNLOAD_COMPLETE, 0, (LPARAM)pErr);
        return 0;
    }

    if (g_CancelDownload) {
        std::string* pErr = new std::string("Download Cancelled");
        PostMessage(hWnd, WM_DOWNLOAD_COMPLETE, 0, (LPARAM)pErr);
        return 0;
    }

    // 2. Fetch and Download
    std::string* pStatus = new std::string("Connecting to YouTube...");
    PostMessage(hWnd, WM_DOWNLOAD_PROGRESS, 0, (LPARAM)pStatus);

    DownloadVideo(hWnd, g_CurrentUrl, g_CurrentQuality, g_CurrentMp3, g_CurrentPlaylist, g_CancelDownload);
    return 0;
}

// Main Window Procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            // Enable visual styles
            INITCOMMONCONTROLSEX icex;
            icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
            icex.dwICC = ICC_PROGRESS_CLASS;
            InitCommonControlsEx(&icex);

            // Default theme is Dark Mode
            g_Theme = CreateDarkTheme();
            SetWindowDarkHeader(hWnd, TRUE);

            // Create fonts
            g_hFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, 
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, 
                                  DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

            // URL Label
            HWND hUrlLbl = CreateWindowExA(0, "STATIC", "Paste YouTube URL:", WS_CHILD | WS_VISIBLE, 
                                           20, 80, 200, 20, hWnd, NULL, NULL, NULL);
            SendMessage(hUrlLbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // URL Edit Control
            g_hUrlEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 
                                         20, 105, 440, 26, hWnd, (HMENU)IDC_URL_EDIT, NULL, NULL);
            SendMessage(g_hUrlEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Paste Button
            g_hPasteBtn = CreateWindowExA(0, "BUTTON", "Paste URL", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                                          470, 105, 90, 26, hWnd, (HMENU)IDC_PASTE_BTN, NULL, NULL);
            SendMessage(g_hPasteBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Dark Mode Checkbox (Top-Right)
            g_hThemeChk = CreateWindowExA(0, "BUTTON", "", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 
                                          460, 78, 20, 20, hWnd, (HMENU)IDC_THEME_CHK, NULL, NULL);
            SendMessage(g_hThemeChk, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(g_hThemeChk, BM_SETCHECK, BST_CHECKED, 0); // Check by default
            
            HWND hThemeLbl = CreateWindowExA(0, "STATIC", "Dark Mode", WS_CHILD | WS_VISIBLE | SS_NOTIFY, 
                                             485, 78, 75, 20, hWnd, (HMENU)IDC_THEME_LBL, NULL, NULL);
            SendMessage(hThemeLbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Quality Label
            HWND hQualLbl = CreateWindowExA(0, "STATIC", "Select Format / Quality:", WS_CHILD | WS_VISIBLE, 
                                            20, 145, 200, 20, hWnd, NULL, NULL, NULL);
            SendMessage(hQualLbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Quality Dropdown
            g_hCombo = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 
                                       20, 170, 300, 300, hWnd, (HMENU)IDC_QUALITY_COMBO, NULL, NULL);
            SendMessage(g_hCombo, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            
            SendMessageA(g_hCombo, CB_ADDSTRING, 0, (LPARAM)"Best Quality (Video+Audio)");
            SendMessageA(g_hCombo, CB_ADDSTRING, 0, (LPARAM)"1080p Full HD (MP4)");
            SendMessageA(g_hCombo, CB_ADDSTRING, 0, (LPARAM)"720p HD (MP4)");
            SendMessageA(g_hCombo, CB_ADDSTRING, 0, (LPARAM)"480p SD (MP4)");
            SendMessageA(g_hCombo, CB_ADDSTRING, 0, (LPARAM)"MP3 Audio (High 320kbps)");
            SendMessageA(g_hCombo, CB_ADDSTRING, 0, (LPARAM)"MP3 Audio (Standard 192kbps)");
            SendMessageA(g_hCombo, CB_ADDSTRING, 0, (LPARAM)"M4A Audio");
            SendMessage(g_hCombo, CB_SETCURSEL, 0, 0);

            // MP3 Extraction Checkbox
            g_hMp3Chk = CreateWindowExA(0, "BUTTON", "", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 
                                        340, 170, 20, 20, hWnd, (HMENU)IDC_MP3_CHK, NULL, NULL);
            SendMessage(g_hMp3Chk, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            HWND hMp3Lbl = CreateWindowExA(0, "STATIC", "Extract Audio Only", WS_CHILD | WS_VISIBLE | SS_NOTIFY, 
                                           365, 170, 200, 20, hWnd, (HMENU)IDC_MP3_LBL, NULL, NULL);
            SendMessage(hMp3Lbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Playlist Checkbox
            g_hPlaylistChk = CreateWindowExA(0, "BUTTON", "", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 
                                             340, 195, 20, 20, hWnd, (HMENU)IDC_PLAYLIST_CHK, NULL, NULL);
            SendMessage(g_hPlaylistChk, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            HWND hPlaylistLbl = CreateWindowExA(0, "STATIC", "Download Full Playlist", WS_CHILD | WS_VISIBLE | SS_NOTIFY, 
                                                365, 195, 200, 20, hWnd, (HMENU)IDC_PLAYLIST_LBL, NULL, NULL);
            SendMessage(hPlaylistLbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Start Download Button
            g_hDownloadBtn = CreateWindowExA(0, "BUTTON", "START DOWNLOAD", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                                             20, 235, 430, 40, hWnd, (HMENU)IDC_DOWNLOAD_BTN, NULL, NULL);
            SendMessage(g_hDownloadBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Cancel Button
            g_hCancelBtn = CreateWindowExA(0, "BUTTON", "CANCEL", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, 
                                           460, 235, 100, 40, hWnd, (HMENU)IDC_CANCEL_BTN, NULL, NULL);
            SendMessage(g_hCancelBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Status Label
            g_hStatusLabel = CreateWindowExA(0, "STATIC", "Status: Ready", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 
                                             20, 290, 540, 20, hWnd, NULL, NULL, NULL);
            SendMessage(g_hStatusLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Progress Bar
            g_hProgressBar = CreateWindowExA(0, PROGRESS_CLASS, "", WS_CHILD | WS_VISIBLE, 
                                             20, 315, 540, 25, hWnd, (HMENU)IDC_PROGRESS_BAR, NULL, NULL);

            // History Label
            HWND hHistLbl = CreateWindowExA(0, "STATIC", "Download History:", WS_CHILD | WS_VISIBLE, 
                                            20, 360, 200, 20, hWnd, NULL, NULL, NULL);
            SendMessage(hHistLbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // History ListBox
            g_hHistoryList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "", 
                                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, 
                                             20, 385, 540, 160, hWnd, (HMENU)IDC_HISTORY_LIST, NULL, NULL);
            SendMessage(g_hHistoryList, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Show Downloads Button
            g_hShowDownloadsBtn = CreateWindowExA(0, "BUTTON", "Show Downloads Folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                                                 20, 555, 160, 25, hWnd, (HMENU)IDC_SHOW_DOWNLOADS_BTN, NULL, NULL);
            SendMessage(g_hShowDownloadsBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Check for Updates Button (beside Clear History)
            g_hCheckUpdatesBtn = CreateWindowExA(0, "BUTTON", "Check for Updates", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                                                295, 555, 135, 25, hWnd, (HMENU)IDC_CHECK_UPDATES_BTN, NULL, NULL);
            SendMessage(g_hCheckUpdatesBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Clear History Button
            g_hClearHistBtn = CreateWindowExA(0, "BUTTON", "Clear History", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                                              440, 555, 120, 25, hWnd, (HMENU)IDC_CLEAR_HIST_BTN, NULL, NULL);
            SendMessage(g_hClearHistBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Populate History
            RefreshHistoryList(g_hHistoryList);
            return 0;
        }

        case WM_COMMAND: {
            UINT wmId    = LOWORD(wParam);   // LOWORD returns WORD; use UINT to avoid sign issues
            UINT wmEvent = HIWORD(wParam);

            switch (wmId) {
                case IDC_PASTE_BTN: {
                    PasteFromClipboard(g_hUrlEdit);
                    break;
                }

                case IDC_THEME_CHK: {
                    if (wmEvent == BN_CLICKED) {
                        BOOL isDark = (SendMessage(g_hThemeChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        ToggleTheme(hWnd, isDark);
                    }
                    break;
                }

                case IDC_THEME_LBL: {
                    if (wmEvent == STN_CLICKED) {
                        BOOL isChecked = (SendMessage(g_hThemeChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        SendMessage(g_hThemeChk, BM_SETCHECK, isChecked ? BST_UNCHECKED : BST_CHECKED, 0);
                        ToggleTheme(hWnd, !isChecked);
                    }
                    break;
                }

                case IDC_QUALITY_COMBO: {
                    if (wmEvent == CBN_SELCHANGE) {
                        int index = (int)SendMessage(g_hCombo, CB_GETCURSEL, 0, 0);
                        // Auto check Extract Audio check if audio is selected in dropdown
                        if (index >= 4) {
                            SendMessage(g_hMp3Chk, BM_SETCHECK, BST_CHECKED, 0);
                        } else {
                            SendMessage(g_hMp3Chk, BM_SETCHECK, BST_UNCHECKED, 0);
                        }
                    }
                    break;
                }

                case IDC_MP3_CHK: {
                    if (wmEvent == BN_CLICKED) {
                        BOOL chk = (SendMessage(g_hMp3Chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        int index = (int)SendMessage(g_hCombo, CB_GETCURSEL, 0, 0);
                        if (chk && index < 4) {
                            // If user checks Extract Audio but video is selected, switch selection to MP3 320k
                            SendMessage(g_hCombo, CB_SETCURSEL, 4, 0);
                        } else if (!chk && index >= 4) {
                            // Switch back to Best Quality
                            SendMessage(g_hCombo, CB_SETCURSEL, 0, 0);
                        }
                    }
                    break;
                }

                case IDC_MP3_LBL: {
                    if (wmEvent == STN_CLICKED) {
                        BOOL isChecked = (SendMessage(g_hMp3Chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        SendMessage(g_hMp3Chk, BM_SETCHECK, isChecked ? BST_UNCHECKED : BST_CHECKED, 0);
                        
                        BOOL chk = !isChecked;
                        int index = (int)SendMessage(g_hCombo, CB_GETCURSEL, 0, 0);
                        if (chk && index < 4) {
                            SendMessage(g_hCombo, CB_SETCURSEL, 4, 0);
                        } else if (!chk && index >= 4) {
                            SendMessage(g_hCombo, CB_SETCURSEL, 0, 0);
                        }
                    }
                    break;
                }

                case IDC_PLAYLIST_LBL: {
                    if (wmEvent == STN_CLICKED) {
                        BOOL isChecked = (SendMessage(g_hPlaylistChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        SendMessage(g_hPlaylistChk, BM_SETCHECK, isChecked ? BST_UNCHECKED : BST_CHECKED, 0);
                    }
                    break;
                }

                case IDC_DOWNLOAD_BTN: {
                    if (g_Downloading) break;

                    char urlBuf[2048];
                    GetWindowTextA(g_hUrlEdit, urlBuf, sizeof(urlBuf));
                    std::string url(urlBuf);

                    // Basic verification
                    if (url.empty() || url.find("http") == std::string::npos) {
                        MessageBoxA(hWnd, "Please enter a valid YouTube URL.", "Invalid URL", MB_OK | MB_ICONWARNING);
                        break;
                    }

                    g_CurrentUrl = url;
                    g_CurrentQuality = (int)SendMessage(g_hCombo, CB_GETCURSEL, 0, 0);
                    g_CurrentMp3 = (SendMessage(g_hMp3Chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    g_CurrentPlaylist = (SendMessage(g_hPlaylistChk, BM_GETCHECK, 0, 0) == BST_CHECKED);

                    // Adjust visual state
                    g_Downloading = true;
                    g_CancelDownload = false;
                    EnableWindow(g_hDownloadBtn, FALSE);
                    EnableWindow(g_hPasteBtn, FALSE);
                    EnableWindow(g_hCombo, FALSE);
                    EnableWindow(g_hMp3Chk, FALSE);
                    EnableWindow(g_hPlaylistChk, FALSE);
                    EnableWindow(g_hClearHistBtn, FALSE);
                    EnableWindow(g_hShowDownloadsBtn, FALSE);
                    EnableWindow(g_hCheckUpdatesBtn, FALSE);
                    EnableWindow(g_hCancelBtn, TRUE);

                    SendMessage(g_hProgressBar, PBM_SETPOS, 0, 0);
                    SetWindowTextA(g_hStatusLabel, "Status: Initializing download stream...");

                    // Launch thread natively and close handle immediately to prevent leaks
                    HANDLE hThread = CreateThread(NULL, 0, DownloadThreadProc, hWnd, 0, NULL);
                    if (hThread) {
                        CloseHandle(hThread);
                    }
                    break;
                }

                case IDC_CANCEL_BTN: {
                    if (!g_Downloading) break;
                    g_CancelDownload = true;
                    SetWindowTextA(g_hStatusLabel, "Status: Cancelling download...");
                    EnableWindow(g_hCancelBtn, FALSE);
                    break;
                }

                case IDC_CLEAR_HIST_BTN: {
                    if (MessageBoxA(hWnd, "Are you sure you want to clear your download history?", "Clear History", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        ClearHistoryFile();
                        RefreshHistoryList(g_hHistoryList);
                    }
                    break;
                }

                case IDC_CHECK_UPDATES_BTN: {
                    if (g_Downloading) break;
                    EnableWindow(g_hCheckUpdatesBtn, FALSE);
                    SetWindowTextA(g_hStatusLabel, "Status: Checking for updates to yt-dlp & FFmpeg...");
                    HANDLE hUpdThread = CreateThread(NULL, 0, UpdateCheckThreadProc, hWnd, 0, NULL);
                    if (hUpdThread) {
                        CloseHandle(hUpdThread);
                    }
                    break;
                }

                case IDC_SHOW_DOWNLOADS_BTN: {
                    std::string dlDir = GetDownloadsDir();
                    ShellExecuteA(NULL, "open", dlDir.c_str(), NULL, NULL, SW_SHOWDEFAULT);
                    break;
                }
            }
            return 0;
        }

        case WM_DOWNLOAD_PROGRESS: {
            int progress = (int)(UINT_PTR)wParam;  // WPARAM is unsigned; safe narrowing via UINT_PTR
            std::string* pStatus = (std::string*)lParam;
            if (pStatus) {
                std::string statusText = "Status: " + *pStatus;
                SetWindowTextA(g_hStatusLabel, statusText.c_str());
                delete pStatus;
            }
            SendMessage(g_hProgressBar, PBM_SETPOS, (WPARAM)progress, 0);
            return 0;
        }

        case WM_DOWNLOAD_COMPLETE: {
            int success = (int)wParam;
            std::string* pMsg = (std::string*)lParam;
            
            g_Downloading = false;
            EnableWindow(g_hDownloadBtn, TRUE);
            EnableWindow(g_hPasteBtn, TRUE);
            EnableWindow(g_hCombo, TRUE);
            EnableWindow(g_hMp3Chk, TRUE);
            EnableWindow(g_hPlaylistChk, TRUE);
            EnableWindow(g_hClearHistBtn, TRUE);
            EnableWindow(g_hShowDownloadsBtn, TRUE);
            EnableWindow(g_hCheckUpdatesBtn, TRUE);
            EnableWindow(g_hCancelBtn, FALSE);

            std::string qualStr = GetQualityLabel(g_CurrentQuality);

            if (success) {
                // Workaround to bypass Win32 progress bar animation delay and fill the gap immediately
                SendMessage(g_hProgressBar, PBM_SETRANGE32, 0, 101);
                SendMessage(g_hProgressBar, PBM_SETPOS, 101, 0);
                SendMessage(g_hProgressBar, PBM_SETPOS, 100, 0);
                SendMessage(g_hProgressBar, PBM_SETRANGE32, 0, 100);

                std::string title = (pMsg) ? *pMsg : "YouTube Video";
                std::string dlFolder = GetDownloadsDir();
                SetWindowTextA(g_hStatusLabel, "Status: Download complete!");
                MessageBoxA(hWnd, ("Download successfully finished!\n\nSaved to:\n" + dlFolder).c_str(), "Success", MB_OK | MB_ICONINFORMATION);
                
                AddHistoryEntry(title, qualStr, "Success");
            } else {
                std::string errStr = (pMsg) ? *pMsg : "Unknown error";
                std::string statusText = "Status: Download failed (" + errStr + ")";
                SetWindowTextA(g_hStatusLabel, statusText.c_str());
                MessageBoxA(hWnd, ("Download failed:\n" + errStr).c_str(), "Error", MB_OK | MB_ICONERROR);
                
                AddHistoryEntry("Failed Download", qualStr, "Failed");
            }

            if (pMsg) {
                delete pMsg;
            }

            RefreshHistoryList(g_hHistoryList);
            return 0;
        }

        case WM_UPDATE_CHECK_DONE: {
            EnableWindow(g_hCheckUpdatesBtn, TRUE);
            SetWindowTextA(g_hStatusLabel, "Status: Ready");
            std::string* pReport = (std::string*)lParam;
            if (pReport) {
                MessageBoxA(hWnd, pReport->c_str(), "Engine Updates", MB_OK | MB_ICONINFORMATION);
                delete pReport;
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rect;
            GetClientRect(hWnd, &rect);
            int clientWidth = rect.right - rect.left;

            // Draw Header Gradient
            TRIVERTEX vertex[2];
            vertex[0].x     = 0;
            vertex[0].y     = 0;
            // Cyan: RGB(0, 180, 255)
            vertex[0].Red   = 0x0000;
            vertex[0].Green = 0xB400;
            vertex[0].Blue  = 0xFF00;
            vertex[0].Alpha = 0x0000;

            vertex[1].x     = clientWidth;
            vertex[1].y     = 60;
            // Purple: RGB(140, 50, 255)
            vertex[1].Red   = 0x8C00;
            vertex[1].Green = 0x3200;
            vertex[1].Blue  = 0xFF00;
            vertex[1].Alpha = 0x0000;

            GRADIENT_RECT gRect;
            gRect.UpperLeft  = 0;
            gRect.LowerRight = 1;

            GradientFill(hdc, vertex, 2, &gRect, 1, GRADIENT_FILL_RECT_H);

            // Draw Header Title Text
            HFONT hTitleFont = CreateFontA(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, 
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, 
                                          DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            HFONT hOldFont = (HFONT)SelectObject(hdc, hTitleFont);

            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkMode(hdc, TRANSPARENT);

            RECT titleRect = { 20, 16, clientWidth - 20, 50 };
            DrawTextA(hdc, "VIVID YOUTUBE DOWNLOADER", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, hOldFont);
            DeleteObject(hTitleFont);

            // Draw Background client area below header
            RECT bgRect = { 0, 60, clientWidth, rect.bottom };
            FillRect(hdc, &bgRect, g_Theme.hbrWindow);

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, g_Theme.fgText);
            SetBkColor(hdc, g_Theme.bgControl);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)g_Theme.hbrControl;  // WM_CTLCOLORxxx must return LRESULT (HBRUSH)
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, g_Theme.fgEdit);
            SetBkColor(hdc, g_Theme.bgEdit);
            return (LRESULT)g_Theme.hbrEdit;  // WM_CTLCOLORxxx must return LRESULT (HBRUSH)
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, g_Theme.fgEdit);
            SetBkColor(hdc, g_Theme.bgEdit);
            return (LRESULT)g_Theme.hbrEdit;  // WM_CTLCOLORxxx must return LRESULT (HBRUSH)
        }

        case WM_ERASEBKGND:
            return 1; // Handled in WM_PAINT

        case WM_DESTROY: {
            if (g_Downloading) {
                g_CancelDownload = true; // Signal thread to terminate background processes immediately
            }
            FreeTheme(g_Theme);
            if (g_hFont) DeleteObject(g_hFont);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

// Window Entry
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; // Suppress unused parameter warning
    (void)lpCmdLine;     // Suppress unused parameter warning
    // Window Registration
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; // Handled dynamically in WM_PAINT
    wc.lpszClassName = "VividYTDownloaderClass";
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(101));
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(101));

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Window Registration Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    // Set fixed width and height (600x630 Window Size)
    // Adjust window rect based on styles to ensure client rect size is correct
    RECT wr = {0, 0, 595, 630};
    AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    HWND hWnd = CreateWindowExA(
        0,
        "VividYTDownloaderClass",
        "Vivid YouTube Downloader",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, hInstance, NULL
    );

    if (hWnd == NULL) {
        MessageBoxA(NULL, "Window Creation Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    g_hWnd = hWnd;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
