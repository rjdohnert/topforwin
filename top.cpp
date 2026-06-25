#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <thread>
#include <iomanip>

// Structure to store CPU time metrics for calculations
struct ProcessTimeSample {
    ULONGLONG kernelTime = 0;
    ULONGLONG userTime = 0;
};

// Structure to hold processed display data
struct ProcessDisplayInfo {
    DWORD pid;
    std::wstring name;
    double cpuPercent;
    SIZE_T memoryUsageBytes;
};

// Enum to manage the current sort order of the process list
enum class SortColumn {
    CPU,
    Memory,
    PID
};

// Global constants
constexpr int MAX_DISPLAY_PROCESSES = 25; // Number of processes to show in the list
constexpr int UPDATE_INTERVAL_MS = 1000;  // Update frequency (1 second)
constexpr int PROCESS_NAME_WIDTH = 30;

// Convert FILETIME to 64-bit unsigned integer
ULONGLONG FileTimeToULL(const FILETIME& ft) {
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

// Resets console cursor position to avoid screen flickering
void ResetCursorPosition() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD coord = { 0, 0 };
    SetConsoleCursorPosition(hOut, coord);
}

void SetCursorPosition(short x, short y) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD coord = { x, y };
    SetConsoleCursorPosition(hOut, coord);
}

// Clear the screen once at initialization
void ClearScreen() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD count;
    DWORD cellCount;
    COORD homeCoords = { 0, 0 };

    if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return;
    cellCount = csbi.dwSize.X * csbi.dwSize.Y;

    FillConsoleOutputCharacter(hOut, (TCHAR)' ', cellCount, homeCoords, &count);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, cellCount, homeCoords, &count);
    SetConsoleCursorPosition(hOut, homeCoords);
}

void PrintStaticHeaders() {
    // Print static labels that will not be redrawn in the loop
    std::wcout << std::left
        << std::setw(8) << L"PID"
        << std::setw(PROCESS_NAME_WIDTH) << L"Process Name"
        << std::setw(12) << L"CPU %"
        << std::setw(15) << L"Memory (MB)"
        << L"\n";
    std::wcout << L"----------------------------------------------------------------------\n";
    std::wcout << L"Sort: (C)PU, (M)emory, (P)ID | (Q)uit" << std::endl;
    std::wcout.flush();
}

void PrintProcessList(const std::vector<ProcessDisplayInfo>& processList, int startLine, SortColumn currentSort) {
    int displayedCount = 0;
    for (const auto& proc : processList) {
        if (displayedCount >= MAX_DISPLAY_PROCESSES) break;

        double memoryMB = static_cast<double>(proc.memoryUsageBytes) / (1024 * 1024);

        std::wstring truncatedName = proc.name;
        if (truncatedName.length() > PROCESS_NAME_WIDTH - 2) {
            truncatedName = truncatedName.substr(0, PROCESS_NAME_WIDTH - 5) + L"...";
        }

        SetCursorPosition(0, startLine + displayedCount);
        std::wcout << std::left
            << std::setw(8) << proc.pid
            << std::setw(PROCESS_NAME_WIDTH) << truncatedName
            << std::setw(12) << std::fixed << std::setprecision(1) << proc.cpuPercent
            << std::setw(15) << std::fixed << std::setprecision(1) << memoryMB
            << L"\n";

        displayedCount++;
    }

    // Pad the bottom of the console in case the list shortens to prevent ghost characters
    for (int i = displayedCount; i < MAX_DISPLAY_PROCESSES; ++i) {
        SetCursorPosition(0, startLine + i);
        std::wcout << std::wstring(80, L' '); // Clear the entire line
    }
}

void CollectProcessInfo(
    std::vector<ProcessDisplayInfo>& processList,
    std::unordered_map<DWORD, ProcessTimeSample>& currentSamples,
    const std::unordered_map<DWORD, ProcessTimeSample>& previousSamples,
    ULONGLONG systemDelta
) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Failed to create process snapshot.\n";
        return;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(hSnapshot, &pe32)) {
        CloseHandle(hSnapshot);
        return;
    }

    do {
        DWORD pid = pe32.th32ProcessID;
        if (pid == 0) continue; // Skip Idle Process

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProcess == NULL) {
            processList.push_back({ pid, pe32.szExeFile, 0.0, 0 });
            continue;
        }

        PROCESS_MEMORY_COUNTERS pmc = {};
        if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
            // Memory info gathered
        }

        FILETIME creationFT, exitFT, kernelFT, userFT;
        double cpuPercent = 0.0;
        if (GetProcessTimes(hProcess, &creationFT, &exitFT, &kernelFT, &userFT)) {
            ULONGLONG currProcKernel = FileTimeToULL(kernelFT);
            ULONGLONG currProcUser = FileTimeToULL(userFT);
            currentSamples[pid] = { currProcKernel, currProcUser };

            if (auto it = previousSamples.find(pid); it != previousSamples.end() && systemDelta > 0) {
                ULONGLONG procDelta = (currProcKernel - it->second.kernelTime) + (currProcUser - it->second.userTime);
                cpuPercent = (static_cast<double>(procDelta) / static_cast<double>(systemDelta)) * 100.0;
            }
        }
        CloseHandle(hProcess);
        processList.push_back({ pid, pe32.szExeFile, cpuPercent, pmc.WorkingSetSize });
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
}

int main() {
    // Set console code page to UTF-8 for wide character support output
    SetConsoleOutputCP(CP_UTF8);
    ClearScreen();
    PrintStaticHeaders();

    // Store historic timing data to calculate delta CPU usage
    std::unordered_map<DWORD, ProcessTimeSample> previousSamples;
    ULONGLONG prevSysKernel = 0;
    ULONGLONG prevSysUser = 0;
    SortColumn currentSort = SortColumn::CPU;

    // Enable virtual terminal sequences for text styling (e.g., highlighting)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);

    while (true) {
        // Non-blocking check for keyboard input to change sort order
        HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
        DWORD numEvents = 0;
        if (GetNumberOfConsoleInputEvents(hInput, &numEvents) && numEvents > 0) {
            INPUT_RECORD record;
            DWORD numRead;
            // Read one event
            if (ReadConsoleInput(hInput, &record, 1, &numRead) && numRead > 0) {
                if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
                    char c = record.Event.KeyEvent.uChar.AsciiChar;
                    switch (std::tolower(c)) {
                    case 'c':
                        currentSort = SortColumn::CPU;
                        break;
                    case 'm':
                        currentSort = SortColumn::Memory;
                        break;
                    case 'p':
                        currentSort = SortColumn::PID;
                        break;
                    case 'q':
                        return 0; // Quit on 'q'
                    }
                }
            }
            // Clear the rest of the input buffer to handle only one key press per frame
            FlushConsoleInputBuffer(hInput);
        }

        // Get current System-wide CPU Times
        FILETIME sysIdleFT, sysKernelFT, sysUserFT;
        if (!GetSystemTimes(&sysIdleFT, &sysKernelFT, &sysUserFT)) {
            std::wcerr << L"Failed to retrieve system times.\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(UPDATE_INTERVAL_MS));
            continue;
        }

        ULONGLONG currSysKernel = FileTimeToULL(sysKernelFT);
        ULONGLONG currSysUser = FileTimeToULL(sysUserFT);
        ULONGLONG systemDelta = (currSysKernel > prevSysKernel && currSysUser > prevSysUser)
            ? (currSysKernel - prevSysKernel) + (currSysUser - prevSysUser)
            : 0;

        // Iterate over processes and gather usage statistics
        std::vector<ProcessDisplayInfo> processList;
        std::unordered_map<DWORD, ProcessTimeSample> currentSamples;
        CollectProcessInfo(processList, currentSamples, previousSamples, systemDelta);

        // Keep historic data updated
        previousSamples = std::move(currentSamples);
        prevSysKernel = currSysKernel;
        prevSysUser = currSysUser;

        // Sort processes based on the current sort column
        std::sort(processList.begin(), processList.end(), [&](const ProcessDisplayInfo& a, const ProcessDisplayInfo& b) {
            switch (currentSort) {
            case SortColumn::Memory:
                if (a.memoryUsageBytes != b.memoryUsageBytes) {
                    return a.memoryUsageBytes > b.memoryUsageBytes;
                }
                break; // Fallback to CPU
            case SortColumn::PID:
                return a.pid < b.pid; // Sort PID ascending
            case SortColumn::CPU:
            default:
                if (a.cpuPercent != b.cpuPercent) {
                    return a.cpuPercent > b.cpuPercent;
                }
                break; // Fallback to Memory
            }
            // Default secondary sort for CPU and Memory is the other metric
            return (currentSort == SortColumn::CPU) ? a.memoryUsageBytes > b.memoryUsageBytes : a.cpuPercent > b.cpuPercent;
        });

        // Render dynamic data to screen
        PrintProcessList(processList, 3, currentSort); // Process list starts on line 3

        std::this_thread::sleep_for(std::chrono::milliseconds(UPDATE_INTERVAL_MS));
    }

    return 0;
}