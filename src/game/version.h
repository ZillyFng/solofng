/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_VERSION_H
#define GAME_VERSION_H
#include <generated/nethash.cpp>
#define GAME_VERSION "0.7.4"
#define GAME_NETVERSION_HASH_FORCED "802f1be60a05665f"
#define GAME_NETVERSION "0.7 " GAME_NETVERSION_HASH_FORCED
#define CLIENT_VERSION 0x0704
#define FNG_VERSION_LEN 16
#define FNG_MAGIC_LEN 4
static const char FNG_VERSION[FNG_VERSION_LEN] = "1.2.0-dev";
static const char FNG_MAGIC[FNG_MAGIC_LEN] = "FNG";
#define SETTINGS_FILENAME "settings07"
static const char GAME_RELEASE_VERSION[8] = "0.7.4";
#endif
