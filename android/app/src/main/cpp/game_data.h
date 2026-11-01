#pragma once
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <stdexcept>
#include <android/log.h>

constexpr const char* kGameRoot = "/storage/emulated/0/nfsu1";
inline void checkGameData() {
    const std::string root(kGameRoot);
    // The PC install names it Speed.exe; a case-sensitive card (ext4) may hold any spelling.
    std::string exePath;
    FILE* exe = nullptr;
    for (const char* name : {"/Speed.exe", "/SPEED.EXE", "/speed.exe"}) {
        exePath = root + name;
        if ((exe = std::fopen(exePath.c_str(), "rb"))) break;
    }
    if (!exe) throw std::runtime_error("Cannot read Internal storage/nfsu1/Speed.exe; check files and storage permission");
    unsigned char magic[2]{};
    const size_t read = std::fread(magic, 1, 2, exe);
    std::fclose(exe);
    struct stat info{};
    if (read != 2 || magic[0] != 'M' || magic[1] != 'Z' ||
        stat(exePath.c_str(), &info) || info.st_size != 3178496)  // Speed.exe 1.4
        throw std::runtime_error("Speed.exe does not match expected image size/header");
    // NFSU1's data folders (the storage is case-insensitive: Cars, FrontEnd, Global...).
    for (const char* name : {"CARS", "FRONTEND", "GLOBAL", "LANGUAGES", "MOVIES", "SOUND", "TRACKS"}) {
        const auto path = root + "/" + name;
        DIR* dir = opendir(path.c_str());
        if (!dir) throw std::runtime_error("Cannot read game directory: " + path);
        unsigned files = 0;
        while (auto* entry = readdir(dir)) if (entry->d_name[0] != '.') ++files;
        closedir(dir);
        if (!files) throw std::runtime_error("Empty game directory: " + path);
        __android_log_print(ANDROID_LOG_INFO, "NFSU1", "Data directory %s: %u entries", name, files);
    }
    __android_log_print(ANDROID_LOG_INFO, "NFSU1", "Game data readable at %s; executable header/size OK (guest not running)", kGameRoot);
}
