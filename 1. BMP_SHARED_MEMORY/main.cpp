#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#pragma pack(push, 1)
struct BmpFileHeader {
    WORD type;
    DWORD fileSize;
    WORD reserved1;
    WORD reserved2;
    DWORD pixelOffset;
};

struct BmpInfoHeader {
    DWORD headerSize;
    LONG width;
    LONG height;
    WORD planes;
    WORD bitsPerPixel;
    DWORD compression;
    DWORD imageSize;
    LONG xPixelsPerMeter;
    LONG yPixelsPerMeter;
    DWORD colorsUsed;
    DWORD colorsImportant;
};

struct BmpPaletteEntry {
    BYTE blue;
    BYTE green;
    BYTE red;
    BYTE reserved;
};
#pragma pack(pop)

struct Rgb {
    BYTE red;
    BYTE green;
    BYTE blue;

    bool operator<(const Rgb& other) const {
        if (red != other.red) return red < other.red;
        if (green != other.green) return green < other.green;
        return blue < other.blue;
    }
};

struct BmpInfo {
    BmpFileHeader fileHeader{};
    BmpInfoHeader infoHeader{};
    int width = 0;
    int height = 0;
    size_t stride = 0;
    size_t paletteOffset = 0;
    size_t paletteCount = 0;
};

static bool ReadBmpInfo(const BYTE* data, size_t fileSize, BmpInfo& bmp) {
    if (fileSize < sizeof(BmpFileHeader) + sizeof(BmpInfoHeader))
        return false;

    std::memcpy(&bmp.fileHeader, data, sizeof(bmp.fileHeader));
    std::memcpy(
        &bmp.infoHeader,
        data + sizeof(BmpFileHeader),
        sizeof(bmp.infoHeader));

    const BmpFileHeader& fh = bmp.fileHeader;
    const BmpInfoHeader& ih = bmp.infoHeader;

    if (fh.type != 0x4D42 ||
        ih.headerSize < sizeof(BmpInfoHeader) ||
        ih.width <= 0 ||
        ih.height == 0 ||
        ih.height == LONG_MIN ||
        ih.planes != 1 ||
        ih.bitsPerPixel != 8 ||
        ih.compression != BI_RGB) {
        return false;
    }

    bmp.width = ih.width;
    bmp.height = ih.height < 0 ? -ih.height : ih.height;
    bmp.stride = (static_cast<size_t>(bmp.width) + 3u) & ~size_t(3u);

    if (static_cast<size_t>(bmp.height) >
        (SIZE_MAX - fh.pixelOffset) / bmp.stride) {
        return false;
    }

    const size_t pixelEnd =
        static_cast<size_t>(fh.pixelOffset) +
        bmp.stride * static_cast<size_t>(bmp.height);

    if (fh.pixelOffset > fileSize || pixelEnd > fileSize)
        return false;

    if (ih.headerSize > fileSize - sizeof(BmpFileHeader))
        return false;

    bmp.paletteOffset = sizeof(BmpFileHeader) + ih.headerSize;
    bmp.paletteCount = ih.colorsUsed == 0 ? 256 : ih.colorsUsed;

    if (bmp.paletteCount == 0 ||
        bmp.paletteCount > 256 ||
        bmp.paletteOffset > fileSize ||
        bmp.paletteCount * sizeof(BmpPaletteEntry) >
            fileSize - bmp.paletteOffset ||
        bmp.paletteOffset + bmp.paletteCount * sizeof(BmpPaletteEntry) >
            fh.pixelOffset) {
        return false;
    }

    return true;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2 || argc > 3) {
        std::wcerr << L"Использование: main.exe <файл.bmp> [число_воркеров]\n";
        return 1;
    }

    int workerCount = 3;

    if (argc == 3) {
        try {
            workerCount = std::stoi(argv[2]);
        } catch (...) {
            std::wcerr << L"Некорректное число воркеров.\n";
            return 1;
        }
    }

    if (workerCount < 1 || workerCount > 32) {
        std::wcerr << L"Число воркеров должно быть от 1 до 32.\n";
        return 1;
    }

    HANDLE file = CreateFileW(
        argv[1],
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Не удалось открыть BMP. Код ошибки: "
                   << GetLastError() << L"\n";
        return 1;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) ||
        fileSize.QuadPart <= 0 ||
        static_cast<unsigned long long>(fileSize.QuadPart) > SIZE_MAX) {
        std::wcerr << L"Не удалось определить размер файла.\n";
        CloseHandle(file);
        return 1;
    }

    const size_t mappedSize = static_cast<size_t>(fileSize.QuadPart);

    HANDLE mapping = CreateFileMappingW(
        file, nullptr, PAGE_READWRITE, 0, 0, nullptr);

    if (!mapping) {
        std::wcerr << L"Не удалось создать отображение файла. Код ошибки: "
                   << GetLastError() << L"\n";
        CloseHandle(file);
        return 1;
    }

    BYTE* data = static_cast<BYTE*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));

    if (!data) {
        std::wcerr << L"Не удалось отобразить файл в память. Код ошибки: "
                   << GetLastError() << L"\n";
        CloseHandle(mapping);
        CloseHandle(file);
        return 1;
    }

    BmpInfo bmp;
    if (!ReadBmpInfo(data, mappedSize, bmp)) {
        std::wcerr << L"Файл не является поддерживаемым 8-битным "
                      L"несжатым BMP.\n";
        UnmapViewOfFile(data);
        CloseHandle(mapping);
        CloseHandle(file);
        return 1;
    }

    // Находим папку main.exe и строим полный путь к worker.exe.
    wchar_t mainPathBuffer[MAX_PATH];
    DWORD pathLength =
        GetModuleFileNameW(nullptr, mainPathBuffer, MAX_PATH);

    if (pathLength == 0 || pathLength >= MAX_PATH) {
        std::wcerr << L"Не удалось определить путь к main.exe.\n";
        UnmapViewOfFile(data);
        CloseHandle(mapping);
        CloseHandle(file);
        return 1;
    }

    wchar_t* lastSlash = std::wcsrchr(mainPathBuffer, L'\\');
    wchar_t* lastForwardSlash = std::wcsrchr(mainPathBuffer, L'/');

    if (lastForwardSlash &&
        (!lastSlash || lastForwardSlash > lastSlash)) {
        lastSlash = lastForwardSlash;
    }

    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
    } else {
        mainPathBuffer[0] = L'\0';
    }

    const std::wstring workerPath =
        std::wstring(mainPathBuffer) + L"worker.exe";

    // При закрытии job object Windows завершит оставшиеся дочерние процессы.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);

    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

        if (!SetInformationJobObject(
                job,
                JobObjectExtendedLimitInformation,
                &limits,
                sizeof(limits))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    std::vector<PROCESS_INFORMATION> workers;

    for (int i = 0; i < workerCount; ++i) {
        HANDLE inheritedMapping = nullptr;

        if (!DuplicateHandle(
                GetCurrentProcess(),
                mapping,
                GetCurrentProcess(),
                &inheritedMapping,
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS)) {
            std::wcerr << L"Не удалось подготовить handle отображения. "
                       << L"Код ошибки: " << GetLastError() << L"\n";
            break;
        }

        std::wstring commandLine =
            L"\"" + workerPath + L"\" " +
            std::to_wstring(
                reinterpret_cast<uintptr_t>(inheritedMapping)) +
            L" " + std::to_wstring(mappedSize) +
            L" " + std::to_wstring(bmp.paletteCount);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};

        // Приостанавливаем процесс, чтобы назначить его job object
        // до начала выполнения.
        BOOL created = CreateProcessW(
            workerPath.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,  // наследовать handles
            CREATE_SUSPENDED,
            nullptr,
            nullptr,
            &startup,
            &process);

        if (!created) {
            std::wcerr << L"Не удалось запустить worker.exe. Код ошибки: "
                       << GetLastError() << L"\n";
            CloseHandle(inheritedMapping);
            break;
        }

        if (job && !AssignProcessToJobObject(job, process.hProcess)) {
            std::wcerr << L"Не удалось назначить воркеру job object. "
                       << L"Код ошибки: " << GetLastError() << L"\n";
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(inheritedMapping);
            continue;
        }

        ResumeThread(process.hThread);

        CloseHandle(process.hThread);
        CloseHandle(inheritedMapping);
        workers.push_back(process);
    }

    std::wcout << L"Запущено воркеров: " << workers.size() << L"\n"
               << L"Размер изображения: " << bmp.width << L"x"
               << bmp.height << L"\n"
               << L"Команды: stats x y ширина высота | exit\n";

    std::wstring command;

    while (std::wcout << L"> " && std::getline(std::wcin, command)) {
        if (command == L"exit" || command == L"quit")
            break;

        if (command.rfind(L"stats", 0) != 0) {
            std::wcout << L"Используйте stats или exit.\n";
            continue;
        }

        int x, y, rectWidth, rectHeight;
        wchar_t extra;

        int parsed = swscanf(
            command.c_str(),
            L"stats %d %d %d %d %c",
            &x, &y, &rectWidth, &rectHeight, &extra);

        if (parsed != 4) {
            std::wcout << L"Формат: stats x y ширина высота\n";
            continue;
        }

        if (x < 0 || y < 0 ||
            rectWidth <= 0 || rectHeight <= 0 ||
            x >= bmp.width || y >= bmp.height ||
            rectWidth > bmp.width - x ||
            rectHeight > bmp.height - y) {
            std::wcout << L"Прямоугольник выходит за границы изображения.\n";
            continue;
        }

        const auto* palette =
            reinterpret_cast<const BmpPaletteEntry*>(
                data + bmp.paletteOffset);

        std::map<Rgb, uint64_t> counts;

        for (int row = y; row < y + rectHeight; ++row) {
            const int bmpRow = bmp.infoHeader.height > 0
                ? bmp.height - 1 - row
                : row;

            const BYTE* pixels =
                data + bmp.fileHeader.pixelOffset +
                static_cast<size_t>(bmpRow) * bmp.stride +
                static_cast<size_t>(x);

            for (int col = 0; col < rectWidth; ++col) {
                const BYTE index = pixels[col];

                if (index >= bmp.paletteCount)
                    continue;

                const BmpPaletteEntry& color = palette[index];
                ++counts[{color.red, color.green, color.blue}];
            }
        }

        std::vector<std::pair<Rgb, uint64_t>> sorted(
            counts.begin(), counts.end());

        std::sort(
            sorted.begin(),
            sorted.end(),
            [](const auto& a, const auto& b) {
                if (a.second != b.second)
                    return a.second > b.second;
                return a.first < b.first;
            });

        std::wcout << L"Цвет RGB и количество пикселей:\n";

        const size_t limit = std::min<size_t>(10, sorted.size());

        for (size_t i = 0; i < limit; ++i) {
            const Rgb& color = sorted[i].first;
            std::wcout << L"(" << static_cast<int>(color.red) << L", "
                       << static_cast<int>(color.green) << L", "
                       << static_cast<int>(color.blue) << L"): "
                       << sorted[i].second << L"\n";
        }
    }

    // Завершаем дочерние процессы при выходе из родительской программы.
    for (const PROCESS_INFORMATION& process : workers) {
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
            TerminateProcess(process.hProcess, 0);
    }

    for (const PROCESS_INFORMATION& process : workers) {
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
    }

    if (job)
        CloseHandle(job);

    FlushViewOfFile(data, 0);
    UnmapViewOfFile(data);
    CloseHandle(mapping);
    CloseHandle(file);

    return 0;
}