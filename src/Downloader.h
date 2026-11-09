#pragma once
#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include <windows.h>
#include <string>
#include <atomic>

// Custom windows messages for communicating with the GUI thread
// WM_DOWNLOAD_START is reserved for future use
#define WM_DOWNLOAD_START     (WM_USER + 101)
#define WM_DOWNLOAD_PROGRESS  (WM_USER + 102) // wParam = progress percent (int), lParam = std::string* (status text, caller must delete)
#define WM_DOWNLOAD_COMPLETE  (WM_USER + 103) // wParam = success (1=yes, 0=no),  lParam = std::string* (title on success / error msg on fail, caller must delete)
#define WM_UPDATE_CHECK_DONE  (WM_USER + 104) // wParam = 0, lParam = std::string* (report text, caller must delete)

// Application and portable path helpers
std::string GetAppDir();
std::string GetDownloadsDir();

// Resolve absolute paths to yt-dlp.exe and ffmpeg.exe
std::string GetYtDlpPath();
std::string GetFFmpegPath();

// Check and update yt-dlp and ffmpeg
std::string CheckAndUpdateEngines(HWND hWndParent);

// Download and set up yt-dlp and ffmpeg if missing.
// Posts WM_DOWNLOAD_PROGRESS messages to hWndParent.
// Returns false if cancelled or failed.
bool SetupDependencies(HWND hWndParent, std::atomic<bool>& cancelFlag);

// Run the yt-dlp download process.
// Posts WM_DOWNLOAD_PROGRESS and WM_DOWNLOAD_COMPLETE to hWndParent.
// Returns true on success (exitCode == 0).
bool DownloadVideo(HWND hWndParent,
                   const std::string& url,
                   int qualityIndex,
                   bool forceMp3,
                   bool allowPlaylist,
                   std::atomic<bool>& cancelFlag);

#endif // DOWNLOADER_H
