#include "Downloader.h"
#include <wininet.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstring>

static bool FileExists(const std::string& path) {
    if (path.empty()) return false;
    DWORD attrib = GetFileAttributesA(path.c_str());
    return (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));
}

std::string GetAppDir() {
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0) {
        std::string path(exePath);
        size_t lastSlash = path.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            return path.substr(0, lastSlash);
        }
    }
    return ".";
}

std::string GetDownloadsDir() {
    std::string dir = GetAppDir() + "\\downloads";
    CreateDirectoryA(dir.c_str(), NULL);
    return dir;
}

static std::string GetCentralFolder() {
    char localAppData[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        std::string path = std::string(localAppData) + "\\VividDownloader";
        CreateDirectoryA(path.c_str(), NULL);
        return path;
    }
    return "";
}

std::string GetYtDlpPath() {
    // 1. Portable app directory (highest priority for true portability)
    std::string appDir = GetAppDir();
    std::string appPath = appDir + "\\yt-dlp.exe";
    if (FileExists(appPath)) {
        return appPath;
    }

    // 2. Current working directory
    if (FileExists("yt-dlp.exe")) {
        char absPath[MAX_PATH];
        if (GetFullPathNameA("yt-dlp.exe", MAX_PATH, absPath, NULL) > 0) {
            return std::string(absPath);
        }
        return ".\\yt-dlp.exe";
    }

    // 3. Centralized folder in %LOCALAPPDATA%
    std::string central = GetCentralFolder();
    if (!central.empty()) {
        std::string centralPath = central + "\\yt-dlp.exe";
        if (FileExists(centralPath)) {
            return centralPath;
        }
    }

    // 4. System PATH
    char pathBuffer[MAX_PATH];
    if (SearchPathA(NULL, "yt-dlp", ".exe", MAX_PATH, pathBuffer, NULL) > 0) {
        return std::string(pathBuffer);
    }

    return "";
}

std::string GetFFmpegPath() {
    // 1. Portable app directory (highest priority)
    std::string appDir = GetAppDir();
    std::string appPath = appDir + "\\ffmpeg.exe";
    if (FileExists(appPath)) {
        return appPath;
    }

    // 2. Current working directory
    if (FileExists("ffmpeg.exe")) {
        char absPath[MAX_PATH];
        if (GetFullPathNameA("ffmpeg.exe", MAX_PATH, absPath, NULL) > 0) {
            return std::string(absPath);
        }
        return ".\\ffmpeg.exe";
    }

    // 3. Centralized folder in %LOCALAPPDATA%
    std::string central = GetCentralFolder();
    if (!central.empty()) {
        std::string centralPath = central + "\\ffmpeg.exe";
        if (FileExists(centralPath)) {
            return centralPath;
        }
    }

    // 4. System PATH
    char pathBuffer[MAX_PATH];
    if (SearchPathA(NULL, "ffmpeg", ".exe", MAX_PATH, pathBuffer, NULL) > 0) {
        return std::string(pathBuffer);
    }

    return "";
}

static bool DownloadFileWinInet(HWND hWndParent, const std::string& url, const std::string& filePath, const std::string& itemName, std::atomic<bool>& cancelFlag) {
    HINTERNET hInternet = InternetOpenA("YT-Downloader-Agent", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        return false;
    }
    
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE | INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTPS;
    HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(), NULL, 0, flags, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return false;
    }

    // Query Content Length
    char szSize[32] = {0};
    DWORD dwSizeLen = sizeof(szSize);
    DWORD totalSize = 0;
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_CONTENT_LENGTH, szSize, &dwSizeLen, NULL)) {
        totalSize = atol(szSize);
    }

    std::ofstream out(filePath, std::ios::binary);
    if (!out.is_open()) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return false;
    }

    char buffer[8192];
    DWORD bytesRead = 0;
    DWORD bytesDownloaded = 0;

    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        if (cancelFlag) {
            out.close();
            DeleteFileA(filePath.c_str());
            InternetCloseHandle(hUrl);
            InternetCloseHandle(hInternet);
            return false;
        }

        out.write(buffer, bytesRead);
        bytesDownloaded += bytesRead;

        int percent = 0;
        if (totalSize > 0) {
            percent = (int)(((double)bytesDownloaded / totalSize) * 100.0);
        }

        // Format progress text
        std::stringstream ss;
        ss << "Downloading " << itemName << "... ";
        if (totalSize > 0) {
            ss << (bytesDownloaded / (1024 * 1024)) << "MB / " << (totalSize / (1024 * 1024)) << "MB (" << percent << "%)";
        } else {
            ss << (bytesDownloaded / (1024 * 1024)) << "MB downloaded";
        }

        std::string* pMsg = new std::string(ss.str());
        PostMessage(hWndParent, WM_DOWNLOAD_PROGRESS, (WPARAM)percent, (LPARAM)pMsg);
    }

    out.close();
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);
    return true;
}

static bool RunCommandHidden(const std::string& command) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    char cmd[8192];
    strncpy(cmd, command.c_str(), sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode == 0;
}

bool SetupDependencies(HWND hWndParent, std::atomic<bool>& cancelFlag) {
    std::string appDir = GetAppDir();
    std::string central = GetCentralFolder();

    // 1. Check if yt-dlp is available
    if (GetYtDlpPath().empty()) {
        std::string* pMsg = new std::string("Downloading yt-dlp core engine...");
        PostMessage(hWndParent, WM_DOWNLOAD_PROGRESS, (WPARAM)0, (LPARAM)pMsg);

        std::string ytDlpUrl = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
        std::string targetExe = appDir + "\\yt-dlp.exe";

        bool downloaded = DownloadFileWinInet(hWndParent, ytDlpUrl, targetExe, "yt-dlp engine", cancelFlag);
        if (!downloaded && !central.empty()) {
            targetExe = central + "\\yt-dlp.exe";
            downloaded = DownloadFileWinInet(hWndParent, ytDlpUrl, targetExe, "yt-dlp engine", cancelFlag);
        }

        if (!downloaded) {
            // Fallback: Try curl.exe if available
            std::string curlCmd = "curl.exe -fL -o \"" + targetExe + "\" \"" + ytDlpUrl + "\"";
            if (!RunCommandHidden(curlCmd) || !FileExists(targetExe)) {
                return false;
            }
        }
    }

    // 2. Check if ffmpeg is available
    if (GetFFmpegPath().empty()) {
        std::string* pMsg = new std::string("Downloading FFmpeg audio/video merger...");
        PostMessage(hWndParent, WM_DOWNLOAD_PROGRESS, (WPARAM)0, (LPARAM)pMsg);

        std::string ffmpegUrl = "https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip";
        std::string targetDir = appDir;
        std::string targetZip = targetDir + "\\ffmpeg.zip";

        bool downloaded = DownloadFileWinInet(hWndParent, ffmpegUrl, targetZip, "FFmpeg component", cancelFlag);
        if (!downloaded && !central.empty()) {
            targetDir = central;
            targetZip = targetDir + "\\ffmpeg.zip";
            downloaded = DownloadFileWinInet(hWndParent, ffmpegUrl, targetZip, "FFmpeg component", cancelFlag);
        }

        if (!downloaded) {
            // Fallback via curl
            std::string curlCmd = "curl.exe -fL -o \"" + targetZip + "\" \"" + ffmpegUrl + "\"";
            if (!RunCommandHidden(curlCmd) || !FileExists(targetZip)) {
                return false;
            }
        }

        pMsg = new std::string("Extracting FFmpeg engine files...");
        PostMessage(hWndParent, WM_DOWNLOAD_PROGRESS, (WPARAM)50, (LPARAM)pMsg);

        std::string extractCmd = "powershell -NoProfile -Command \"Expand-Archive -Path '" + targetZip + "' -DestinationPath '" + targetDir + "\\temp_ffmpeg' -Force; " +
            "Get-ChildItem -Path '" + targetDir + "\\temp_ffmpeg' -Filter 'ffmpeg.exe' -Recurse | Copy-Item -Destination '" + targetDir + "'; " +
            "Remove-Item -Recurse -Force '" + targetDir + "\\temp_ffmpeg', '" + targetZip + "'\"";
        
        if (!RunCommandHidden(extractCmd)) {
            DeleteFileA(targetZip.c_str());
            return false;
        }

        if (GetFFmpegPath().empty()) {
            return false;
        }
    }

    return true;
}

static void ProcessLine(HWND hWndParent, const std::string& line, std::string& videoTitle, std::string& lastErrorMsg) {
    if (line.empty()) return;

    if (line.find("ERROR:") != std::string::npos) {
        lastErrorMsg = line;
    }

    if (line.find("[download]") != std::string::npos) {
        size_t pctPos = line.find('%');
        if (pctPos != std::string::npos) {
            size_t startPos = pctPos;
            while (startPos > 0 && (isdigit(line[startPos - 1]) || line[startPos - 1] == '.' || line[startPos - 1] == ' ')) {
                startPos--;
            }
            std::string pctStr = line.substr(startPos, pctPos - startPos);
            int percent = (int)atof(pctStr.c_str());

            size_t atPos = line.find(" at ");
            size_t etaPos = line.find(" ETA ");
            std::string speed = "---";
            std::string eta = "---";

            if (atPos != std::string::npos && etaPos != std::string::npos) {
                speed = line.substr(atPos + 4, etaPos - (atPos + 4));
                eta = line.substr(etaPos + 5);
                
                // Trim trailing spaces/newlines
                speed.erase(speed.find_last_not_of(" \t\r\n") + 1);
                eta.erase(eta.find_last_not_of(" \t\r\n") + 1);
            }

            std::stringstream ss;
            ss << "Downloading: " << percent << "% | Speed: " << speed << " | ETA: " << eta;
            std::string* pMsg = new std::string(ss.str());
            PostMessage(hWndParent, WM_DOWNLOAD_PROGRESS, (WPARAM)percent, (LPARAM)pMsg);
        } else if (line.find("Destination:") != std::string::npos) {
            size_t destPos = line.find("Destination:");
            std::string dest = line.substr(destPos + 12);
            dest.erase(0, dest.find_first_not_of(" \t"));
            
            size_t lastSlash = dest.find_last_of("\\/");
            std::string filename = (lastSlash == std::string::npos) ? dest : dest.substr(lastSlash + 1);

            size_t lastDot = filename.find_last_of(".");
            std::string title = (lastDot == std::string::npos) ? filename : filename.substr(0, lastDot);
            
            if (videoTitle.empty() || videoTitle == "YouTube Video") {
                videoTitle = title;
            }

            std::string* pMsg = new std::string("Target file: " + filename);
            PostMessage(hWndParent, WM_DOWNLOAD_PROGRESS, (WPARAM)0, (LPARAM)pMsg);
        }
    } else if (line.find("[ExtractAudio]") != std::string::npos) {
        std::string* pMsg = new std::string("Extracting audio and converting to MP3...");
        PostMessage(hWndParent, WM_DOWNLOAD_PROGRESS, (WPARAM)95, (LPARAM)pMsg);
    } else if (line.find("[Merger]") != std::string::npos) {
        std::string* pMsg = new std::string("Merging high-quality video and audio channels...");
        PostMessage(hWndParent, WM_DOWNLOAD_PROGRESS, (WPARAM)95, (LPARAM)pMsg);
    }
}

bool DownloadVideo(HWND hWndParent, 
                  const std::string& url, 
                  int qualityIndex, 
                  bool forceMp3, 
                  bool allowPlaylist, 
                  std::atomic<bool>& cancelFlag) {
                      
    (void)forceMp3; // Suppress unused parameter warning
    std::string downloadsDir = GetDownloadsDir();

    std::string ytDlpPath = GetYtDlpPath();
    std::string ffmpegPath = GetFFmpegPath();

    if (ytDlpPath.empty()) {
        std::string* pErr = new std::string("yt-dlp engine not found. Please verify yt-dlp.exe is present.");
        PostMessage(hWndParent, WM_DOWNLOAD_COMPLETE, 0, (LPARAM)pErr);
        return false;
    }

    std::string cmd = "\"" + ytDlpPath + "\" --newline --no-cache-dir --progress --concurrent-fragments 16 --progress-template \"[download] %(progress._percent_str)s at %(progress._speed_str)s ETA %(progress._eta_str)s\" ";
    
    // Check for Node.js JS runtime to solve YouTube JS challenges seamlessly
    char nodePathBuffer[MAX_PATH];
    if (SearchPathA(NULL, "node", ".exe", MAX_PATH, nodePathBuffer, NULL) > 0 ||
        FileExists("C:\\Program Files\\nodejs\\node.exe")) {
        cmd += "--js-runtimes node ";
    }

    if (!ffmpegPath.empty()) {
        size_t lastSlash = ffmpegPath.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            std::string ffmpegDir = ffmpegPath.substr(0, lastSlash);
            cmd += "--ffmpeg-location \"" + ffmpegDir + "\" ";
        } else {
            cmd += "--ffmpeg-location \"" + ffmpegPath + "\" ";
        }
    }

    if (allowPlaylist) {
        cmd += "--yes-playlist ";
    } else {
        cmd += "--no-playlist ";
    }

    cmd += "-o \"" + downloadsDir + "/%(title)s.%(ext)s\" ";

    switch (qualityIndex) {
        case 0: cmd += "-S \"res,ext:mp4:m4a\" --merge-output-format mp4 "; break;
        case 1: cmd += "-S \"res:1080,ext:mp4:m4a\" --merge-output-format mp4 "; break;
        case 2: cmd += "-S \"res:720,ext:mp4:m4a\" --merge-output-format mp4 "; break;
        case 3: cmd += "-S \"res:480,ext:mp4:m4a\" --merge-output-format mp4 "; break;
        case 4: cmd += "-x --audio-format mp3 --audio-quality 0 "; break;
        case 5: cmd += "-x --audio-format mp3 --audio-quality 5 "; break;
        case 6: cmd += "-f \"bestaudio[ext=m4a]/best\" "; break;
        default: cmd += "-S \"res,ext:mp4:m4a\" --merge-output-format mp4 "; break;
    }

    cmd += "\"" + url + "\"";

    // Setup redirected pipes
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(saAttr);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hChildStd_OUT_Rd = NULL;
    HANDLE hChildStd_OUT_Wr = NULL;

    if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0)) {
        std::string* pErr = new std::string("Failed to create redirection pipe.");
        PostMessage(hWndParent, WM_DOWNLOAD_COMPLETE, 0, (LPARAM)pErr);
        return false;
    }

    if (!SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hChildStd_OUT_Rd);
        CloseHandle(hChildStd_OUT_Wr);
        std::string* pErr = new std::string("Failed to set pipe handle options.");
        PostMessage(hWndParent, WM_DOWNLOAD_COMPLETE, 0, (LPARAM)pErr);
        return false;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hChildStd_OUT_Wr;
    si.hStdOutput = hChildStd_OUT_Wr;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    char cmdLine[8192];
    strncpy(cmdLine, cmd.c_str(), sizeof(cmdLine) - 1);
    cmdLine[sizeof(cmdLine) - 1] = '\0';

    if (!CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hChildStd_OUT_Rd);
        CloseHandle(hChildStd_OUT_Wr);
        std::string* pErr = new std::string("Failed to launch download engine (CreateProcess failed).");
        PostMessage(hWndParent, WM_DOWNLOAD_COMPLETE, 0, (LPARAM)pErr);
        return false;
    }

    // Close the write end of the pipe in parent
    CloseHandle(hChildStd_OUT_Wr);

    char buffer[4096];
    DWORD bytesRead = 0;
    std::string lineBuffer = "";
    std::string videoTitle = "YouTube Video";
    std::string lastErrorMsg = "";

    while (ReadFile(hChildStd_OUT_Rd, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        if (cancelFlag) {
            // Kill child process tree
            TerminateProcess(pi.hProcess, 1);
            
            CloseHandle(hChildStd_OUT_Rd);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            
            std::string* pErr = new std::string("Download Cancelled");
            PostMessage(hWndParent, WM_DOWNLOAD_COMPLETE, 0, (LPARAM)pErr);
            return false;
        }
        buffer[bytesRead] = '\0';
        lineBuffer += buffer;

        size_t pos;
        while ((pos = lineBuffer.find_first_of("\r\n")) != std::string::npos) {
            std::string line = lineBuffer.substr(0, pos);
            lineBuffer.erase(0, pos + 1);
            ProcessLine(hWndParent, line, videoTitle, lastErrorMsg);
        }
    }

    // Process remainder of buffer
    if (!lineBuffer.empty()) {
        ProcessLine(hWndParent, lineBuffer, videoTitle, lastErrorMsg);
    }

    CloseHandle(hChildStd_OUT_Rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode == 0) {
        std::string* pTitle = new std::string(videoTitle);
        PostMessage(hWndParent, WM_DOWNLOAD_COMPLETE, 1, (LPARAM)pTitle);
        return true;
    } else {
        std::string errStr;
        if (!lastErrorMsg.empty()) {
            errStr = lastErrorMsg;
        } else {
            errStr = "yt-dlp exited with error " + std::to_string(exitCode);
        }
        std::string* pErr = new std::string(errStr);
        PostMessage(hWndParent, WM_DOWNLOAD_COMPLETE, 0, (LPARAM)pErr);
        return false;
    }
}

std::string CheckAndUpdateEngines(HWND hWndParent) {
    (void)hWndParent;
    std::string report = "=== Component Status & Update Report ===\n\n";

    // 1. yt-dlp update check
    std::string ytDlpPath = GetYtDlpPath();
    if (ytDlpPath.empty()) {
        report += "[yt-dlp Engine]: Not found in application directory.\n";
    } else {
        report += "[yt-dlp Engine]: Checking for updates...\n";
        std::string cmd = "\"" + ytDlpPath + "\" -U";

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;

        HANDLE hRead = NULL;
        HANDLE hWrite = NULL;

        if (CreatePipe(&hRead, &hWrite, &sa, 0)) {
            SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            si.hStdError = hWrite;
            si.hStdOutput = hWrite;
            si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            ZeroMemory(&pi, sizeof(pi));

            char cmdBuf[2048];
            strncpy(cmdBuf, cmd.c_str(), sizeof(cmdBuf) - 1);
            cmdBuf[sizeof(cmdBuf) - 1] = '\0';

            if (CreateProcessA(NULL, cmdBuf, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                CloseHandle(hWrite);

                char buf[512];
                DWORD dwRead = 0;
                std::string output = "";
                while (ReadFile(hRead, buf, sizeof(buf) - 1, &dwRead, NULL) && dwRead > 0) {
                    buf[dwRead] = '\0';
                    output += buf;
                }

                WaitForSingleObject(pi.hProcess, 30000);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                CloseHandle(hRead);

                if (output.find("is up to date") != std::string::npos) {
                    report += "-> Status: Up to date! You are on the latest version.\n";
                } else if (output.find("Updated yt-dlp") != std::string::npos || output.find("Updating to") != std::string::npos) {
                    report += "-> Status: Successfully updated to latest version!\n";
                } else if (!output.empty()) {
                    report += "-> " + output.substr(0, 150) + "\n";
                } else {
                    report += "-> Status: Up to date.\n";
                }
            } else {
                CloseHandle(hWrite);
                CloseHandle(hRead);
                report += "-> Failed to execute yt-dlp updater.\n";
            }
        }
    }

    report += "\n";

    // 2. FFmpeg check
    std::string ffmpegPath = GetFFmpegPath();
    if (!ffmpegPath.empty()) {
        report += "[FFmpeg Audio/Video Merger]:\n-> Installed & ready at " + ffmpegPath + "\n";
    } else {
        report += "[FFmpeg Audio/Video Merger]:\n-> Missing in application folder.\n";
    }

    report += "\nAll components verified.";
    return report;
}
