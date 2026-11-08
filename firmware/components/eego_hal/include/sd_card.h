// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <esp_err.h>
#include "sdmmc_cmd.h"  // sdmmc_card_t

namespace eego {
class SdCard {
public:
    static SdCard& instance();

    esp_err_t mount();
    void unmount();
    bool ready() const { return mounted_; }

    bool exists(const char* path);
    bool mkdir(const char* path);
    bool remove(const char* path);
    bool rename(const char* from, const char* to);

    struct DirEntry {
        char name[256];
        bool isDir;
        size_t size;
    };
    int listDir(const char* path, DirEntry* entries, int maxEntries);

    FILE* open(const char* path, const char* mode);
    size_t readFile(const char* path, uint8_t* buffer, size_t maxLen);
    bool writeFile(const char* path, const uint8_t* data, size_t len);

    size_t fileSize(const char* path);

private:
    SdCard() = default;
    void makeFullPath(const char* path, char* out, size_t outLen) const;
    esp_err_t mountOnce(uint32_t freqKhz);
    bool mounted_ = false;
    sdmmc_card_t* card_ = nullptr;
};

}  // namespace eego
