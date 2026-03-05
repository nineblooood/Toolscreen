#pragma once

#include <string>


#define TOOLSCREEN_VERSION_MAJOR 80
#define TOOLSCREEN_VERSION_MINOR 1
#define TOOLSCREEN_VERSION_PATCH 0

#define CONFIG_VERSION 2

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define TOOLSCREEN_VERSION_STRING                                                                                                          \
    TOSTRING(TOOLSCREEN_VERSION_MAJOR) "." TOSTRING(TOOLSCREEN_VERSION_MINOR) "." TOSTRING(TOOLSCREEN_VERSION_PATCH)

std::string GetToolscreenVersionString();
int GetConfigVersion();
std::string GetFullVersionInfo();
void LogVersionInfo();
void PrintVersionToStdout();

struct GameVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool valid = false;

    GameVersion() = default;
    GameVersion(int maj, int min, int pat) : major(maj), minor(min), patch(pat), valid(true) {}

    bool operator>=(const GameVersion& other) const {
        if (!valid || !other.valid) return false;
        if (major != other.major) return major > other.major;
        if (minor != other.minor) return minor > other.minor;
        return patch >= other.patch;
    }

    bool operator<=(const GameVersion& other) const {
        if (!valid || !other.valid) return false;
        if (major != other.major) return major < other.major;
        if (minor != other.minor) return minor < other.minor;
        return patch <= other.patch;
    }

    bool operator<(const GameVersion& other) const { return !(*this >= other); }

    bool operator>(const GameVersion& other) const { return !(*this <= other); }
};

GameVersion GetGameVersionFromCommandLine();
GameVersion ParseMinecraftVersionFromMMCPack(const std::wstring& mmcPackPath);
bool IsVersionInRange(const GameVersion& version, const GameVersion& minVer, const GameVersion& maxVer);
bool IsResolutionChangeSupported(const GameVersion& version);


