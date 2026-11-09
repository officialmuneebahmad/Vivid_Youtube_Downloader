#include "History.h"
#include "Downloader.h"
#include <fstream>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <iomanip>

static const char DELIMITER = '\t';

// Helper to replace character
static std::string CleanString(std::string str) {
    std::replace(str.begin(), str.end(), DELIMITER, ' ');
    std::replace(str.begin(), str.end(), '\n', ' ');
    std::replace(str.begin(), str.end(), '\r', ' ');
    return str;
}

std::vector<HistoryEntry> LoadHistory() {
    std::vector<HistoryEntry> history;
    std::ifstream file(GetHistoryFilePath());
    if (!file.is_open()) {
        return history;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string date, title, quality, status;

        if (std::getline(ss, date, DELIMITER) &&
            std::getline(ss, title, DELIMITER) &&
            std::getline(ss, quality, DELIMITER) &&
            std::getline(ss, status, DELIMITER)) {
            history.push_back({date, title, quality, status});
        }
    }
    file.close();
    // Reverse to show newest first
    std::reverse(history.begin(), history.end());
    return history;
}

void SaveHistory(const std::vector<HistoryEntry>& history) {
    std::ofstream file(GetHistoryFilePath(), std::ios::trunc);
    if (!file.is_open()) return;

    for (const auto& entry : history) {
        file << CleanString(entry.dateTime) << DELIMITER
             << CleanString(entry.title) << DELIMITER
             << CleanString(entry.quality) << DELIMITER
             << CleanString(entry.status) << "\n";
    }
    file.close();
}

void AddHistoryEntry(const std::string& title, const std::string& quality, const std::string& status) {
    std::time_t t = std::time(nullptr);
    std::tm* tm_struct = std::localtime(&t);
    if (!tm_struct) return;

    std::stringstream ss;
    ss << std::put_time(tm_struct, "%Y-%m-%d %H:%M:%S");
    std::string dateStr = ss.str();

    std::ofstream file(GetHistoryFilePath(), std::ios::app);
    if (!file.is_open()) return;

    file << CleanString(dateStr) << DELIMITER
         << CleanString(title) << DELIMITER
         << CleanString(quality) << DELIMITER
         << CleanString(status) << "\n";
    file.close();
}

void ClearHistoryFile() {
    std::ofstream file(GetHistoryFilePath(), std::ios::trunc);
    file.close();
}
