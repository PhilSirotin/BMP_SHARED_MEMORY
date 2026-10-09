#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>

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
#pragma pack(pop)

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4) {
        std::wcerr << L"worker: нужны handle, размер файла и размер палитры.\n";
        return 1;
    }

    uintptr_t rawHandle = 0;
    unsigned long long rawSize = 0;
    unsigned long long rawPaletteCount = 0;

    try {
        rawHandle = static_cast<uintptr_t>(std::stoull(argv[1]));
        rawSize = std::stoull(argv[2]);
        rawPaletteCount = std::stoull(argv[3]);
    } catch (...) {
        return 1;
    }

    if (rawHandle == 0 ||
        rawSize < sizeof(BmpFileHeader) + sizeof(BmpInfoHeader) ||
        rawSize > SIZE_MAX ||
        rawPaletteCount == 0 ||
        rawPaletteCount > 256) {
        return 1;
    }

    HANDLE mapping = reinterpret_cast<HANDLE>(rawHandle);
    const size_t fileSize = static_cast<size_t>(rawSize);
    const size_t paletteCount = static_cast<size_t>(rawPaletteCount);

    BYTE* data = static_cast<BYTE*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, fileSize));

    if (!data)
        return 1;

    BmpFileHeader fileHeader{};
    BmpInfoHeader info{};

    std::memcpy(&fileHeader, data, sizeof(fileHeader));
    std::memcpy(
        &info,
        data + sizeof(BmpFileHeader),
        sizeof(info));

    if (fileHeader.type != 0x4D42 ||
        info.headerSize < sizeof(BmpInfoHeader) ||
        info.width <= 0 ||
        info.height == 0 ||
        info.height == LONG_MIN ||
        info.planes != 1 ||
        info.bitsPerPixel != 8 ||
        info.compression != BI_RGB) {
        UnmapViewOfFile(data);
        return 1;
    }

    const int width = info.width;
    const int height = info.height < 0 ? -info.height : info.height;
    const size_t stride =
        (static_cast<size_t>(width) + 3u) & ~size_t(3u);

    if (fileHeader.pixelOffset > fileSize ||
        static_cast<size_t>(height) >
            (fileSize - fileHeader.pixelOffset) / stride) {
        UnmapViewOfFile(data);
        return 1;
    }

    std::mt19937 random(
        static_cast<unsigned int>(GetCurrentProcessId()) ^
        static_cast<unsigned int>(GetTickCount()));

    std::uniform_int_distribution<int> xDist(0, width - 1);
    std::uniform_int_distribution<int> yDist(0, height - 1);
    std::uniform_int_distribution<int> paletteDist(
        0, static_cast<int>(paletteCount - 1));
    std::uniform_int_distribution<int> delayDist(100, 1000);
    std::uniform_int_distribution<int> changesDist(1, 5);

    while (true) {
        const int changes = changesDist(random);

        for (int i = 0; i < changes; ++i) {
            const int x = xDist(random);
            const int y = yDist(random);

            // Положительная высота BMP означает порядок строк снизу вверх.
            const int bmpRow = info.height > 0 ? height - 1 - y : y;

            BYTE* pixel =
                data + fileHeader.pixelOffset +
                static_cast<size_t>(bmpRow) * stride +
                static_cast<size_t>(x);

            // В 8-битном BMP пиксель содержит индекс в цветовой палитре.
            *pixel = static_cast<BYTE>(paletteDist(random));
        }

        FlushViewOfFile(data, 0);
        Sleep(static_cast<DWORD>(delayDist(random)));
    }

    UnmapViewOfFile(data);
    return 0;
}