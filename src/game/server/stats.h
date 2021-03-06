#ifndef GAME_SERVER_STATS_H
#define GAME_SERVER_STATS_H

#include <engine/shared/protocol.h>
#include "time.h"

#define MAX_MULTIS 32
#define MAX_FILE_LEN 1024
#define MAX_FILE_PATH 2048

enum {
	CFG_VANILLA_HAMMER=1,
	CFG_UNUSED0=2,
	CFG_UNUSED1=4,
	CFG_UNUSED2=8,
	CFG_UNUSED3=16,
	CFG_UNUSED4=32,
	CFG_UNUSED5=64,
	MAX_CFG_FLAGS,
};

struct CFngStats {
		char m_aName[MAX_NAME_LENGTH];
		char m_aClan[MAX_CLAN_LENGTH];
		int m_Kills, m_Deaths;
		int m_GoldSpikes, m_GreenSpikes, m_PurpleSpikes;
		int m_RifleShots, m_Freezes, m_Frozen;
		int m_Spree, m_SpreeBest;
		time_t m_LastKillTime;
		int m_Multi, m_MultiBest, m_aMultis[MAX_MULTIS];
		// m_Tmp is never presistet to file and only used in memory
		// so it could use every other intger instead for sorting ranks
		// in case the space is needed keep in mind m_Tmp is basically unused
		int m_Tmp, m_CfgFlags, m_Unused2;
		time_t m_FirstSeen, m_LastSeen, m_TotalOnlineTime;
};

#endif
