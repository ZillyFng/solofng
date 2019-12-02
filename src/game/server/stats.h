#ifndef GAME_SERVER_STATS_H
#define GAME_SERVER_STATS_H

#include <engine/shared/protocol.h>
#include "time.h"

#define MAX_MULTIS 32
#define MAX_IPS 5
#define MAX_IP_LEN 64 // IPv6 should be max 45 but :shrug:

struct CFngStats {
		char m_aName[MAX_NAME_LENGTH];
		char m_aClan[MAX_CLAN_LENGTH];
		char m_aaIP[MAX_IPS][MAX_IP_LEN];
		int m_Kills, m_Deaths;
		int m_GoldSpikes, m_GreenSpikes, m_PurpleSpikes;
		int m_RifleShots, m_Freezes, m_Frozen;
		int m_Spree, m_SpreeBest;
		int m_Multi, m_MultiBest, m_aMultis[MAX_MULTIS];
		int m_Unused0, m_Unused1, m_Unused2;
		time_t m_FirstSeen, m_LastSeen, m_TotalOnlineTime;
};

#endif
