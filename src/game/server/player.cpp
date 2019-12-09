/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "stdio.h"
#include <game/version.h>
#include "entities/character.h"
#include "entities/flag.h"
#include "gamecontext.h"
#include "gamecontroller.h"

#include "player.h"

#if defined(CONF_FAMILY_UNIX)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

MACRO_ALLOC_POOL_ID_IMPL(CPlayer, MAX_CLIENTS)

IServer *CPlayer::Server() const { return m_pGameServer->Server(); }

CPlayer::CPlayer(CGameContext *pGameServer, int ClientID, bool Dummy, bool AsSpec)
{
	m_pGameServer = pGameServer;
	m_RespawnTick = Server()->Tick();
	m_DieTick = Server()->Tick();
	m_ScoreStartTick = Server()->Tick();
	m_pCharacter = 0;
	m_ClientID = ClientID;
	m_Team = AsSpec ? TEAM_SPECTATORS : GameServer()->m_pController->GetStartTeam();
	m_SpecMode = SPEC_FREEVIEW;
	m_SpectatorID = -1;
	m_pSpecFlag = 0;
	m_ActiveSpecSwitch = 0;
	m_LastActionTick = Server()->Tick();
	m_TeamChangeTick = Server()->Tick();
	m_InactivityTickCounter = 0;
	m_Dummy = Dummy;
	m_IsReadyToPlay = !GameServer()->m_pController->IsPlayerReadyMode();
	m_RespawnDisabled = GameServer()->m_pController->GetStartRespawnState();
	m_DeadSpecMode = false;
	m_Spawning = 0;

	// solofng

	m_InitedRoundStats = false;
	m_JoinTime = time(NULL);
}

CPlayer::~CPlayer()
{
	delete m_pCharacter;
	m_pCharacter = 0;
}

void CPlayer::Tick()
{
	if(!IsDummy() && !Server()->ClientIngame(m_ClientID))
		return;

	Server()->SetClientScore(m_ClientID, m_Score);

	// do latency stuff
	{
		IServer::CClientInfo Info;
		if(Server()->GetClientInfo(m_ClientID, &Info))
		{
			m_Latency.m_Accum += Info.m_Latency;
			m_Latency.m_AccumMax = maximum(m_Latency.m_AccumMax, Info.m_Latency);
			m_Latency.m_AccumMin = minimum(m_Latency.m_AccumMin, Info.m_Latency);
		}
		// each second
		if(Server()->Tick()%Server()->TickSpeed() == 0)
		{
			m_Latency.m_Avg = m_Latency.m_Accum/Server()->TickSpeed();
			m_Latency.m_Max = m_Latency.m_AccumMax;
			m_Latency.m_Min = m_Latency.m_AccumMin;
			m_Latency.m_Accum = 0;
			m_Latency.m_AccumMin = 1000;
			m_Latency.m_AccumMax = 0;
		}
	}

	if(m_pCharacter && !m_pCharacter->IsAlive())
	{
		delete m_pCharacter;
		m_pCharacter = 0;
	}

	if(!GameServer()->m_pController->IsGamePaused())
	{
		if(!m_pCharacter && m_Team == TEAM_SPECTATORS && m_SpecMode == SPEC_FREEVIEW)
			m_ViewPos -= vec2(clamp(m_ViewPos.x-m_LatestActivity.m_TargetX, -500.0f, 500.0f), clamp(m_ViewPos.y-m_LatestActivity.m_TargetY, -400.0f, 400.0f));

		if(!m_pCharacter && m_DieTick+Server()->TickSpeed()*3 <= Server()->Tick() && !m_DeadSpecMode)
			Respawn();

		if(!m_pCharacter && m_Team == TEAM_SPECTATORS && m_pSpecFlag)
		{
			if(m_pSpecFlag->GetCarrier())
				m_SpectatorID = m_pSpecFlag->GetCarrier()->GetPlayer()->GetCID();
			else
				m_SpectatorID = -1;
		}

		if(m_pCharacter)
		{
			if(m_pCharacter->IsAlive())
				m_ViewPos = m_pCharacter->GetPos();
		}
		else if(m_Spawning && m_RespawnTick <= Server()->Tick())
			TryRespawn();

		if(!m_DeadSpecMode && m_LastActionTick != Server()->Tick())
			++m_InactivityTickCounter;
	}
	else
	{
		++m_RespawnTick;
		++m_DieTick;
		++m_ScoreStartTick;
		++m_LastActionTick;
		++m_TeamChangeTick;
 	}
}

void CPlayer::PostTick()
{
	// update latency value
	if(m_PlayerFlags&PLAYERFLAG_SCOREBOARD)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
				m_aActLatency[i] = GameServer()->m_apPlayers[i]->m_Latency.m_Min;
		}
	}

	// update view pos for spectators and dead players
	if((m_Team == TEAM_SPECTATORS || m_DeadSpecMode) && m_SpecMode != SPEC_FREEVIEW)
	{
		if(m_pSpecFlag)
			m_ViewPos = m_pSpecFlag->GetPos();
		else if (GameServer()->m_apPlayers[m_SpectatorID])
			m_ViewPos = GameServer()->m_apPlayers[m_SpectatorID]->m_ViewPos;
	}
}

void CPlayer::Snap(int SnappingClient)
{
	if(!IsDummy() && !Server()->ClientIngame(m_ClientID))
		return;

	CNetObj_PlayerInfo *pPlayerInfo = static_cast<CNetObj_PlayerInfo *>(Server()->SnapNewItem(NETOBJTYPE_PLAYERINFO, m_ClientID, sizeof(CNetObj_PlayerInfo)));
	if(!pPlayerInfo)
		return;

	pPlayerInfo->m_PlayerFlags = m_PlayerFlags&PLAYERFLAG_CHATTING;
	if(Server()->IsAuthed(m_ClientID))
		pPlayerInfo->m_PlayerFlags |= PLAYERFLAG_ADMIN;
	if(!GameServer()->m_pController->IsPlayerReadyMode() || m_IsReadyToPlay)
		pPlayerInfo->m_PlayerFlags |= PLAYERFLAG_READY;
	if(m_RespawnDisabled && (!GetCharacter() || !GetCharacter()->IsAlive()))
		pPlayerInfo->m_PlayerFlags |= PLAYERFLAG_DEAD;
	if(SnappingClient != -1 && (m_Team == TEAM_SPECTATORS || m_DeadSpecMode) && (SnappingClient == m_SpectatorID))
		pPlayerInfo->m_PlayerFlags |= PLAYERFLAG_WATCHING;

	pPlayerInfo->m_Latency = SnappingClient == -1 ? m_Latency.m_Min : GameServer()->m_apPlayers[SnappingClient]->m_aActLatency[m_ClientID];
	pPlayerInfo->m_Score = m_Score;

	if(m_ClientID == SnappingClient && (m_Team == TEAM_SPECTATORS || m_DeadSpecMode))
	{
		CNetObj_SpectatorInfo *pSpectatorInfo = static_cast<CNetObj_SpectatorInfo *>(Server()->SnapNewItem(NETOBJTYPE_SPECTATORINFO, m_ClientID, sizeof(CNetObj_SpectatorInfo)));
		if(!pSpectatorInfo)
			return;

		pSpectatorInfo->m_SpecMode = m_SpecMode;
		pSpectatorInfo->m_SpectatorID = m_SpectatorID;
		if(m_pSpecFlag)
		{
			pSpectatorInfo->m_X = m_pSpecFlag->GetPos().x;
			pSpectatorInfo->m_Y = m_pSpecFlag->GetPos().y;
		}
		else
		{
			pSpectatorInfo->m_X = m_ViewPos.x;
			pSpectatorInfo->m_Y = m_ViewPos.y;
		}
	}

	// demo recording
	if(SnappingClient == -1)
	{
		CNetObj_De_ClientInfo *pClientInfo = static_cast<CNetObj_De_ClientInfo *>(Server()->SnapNewItem(NETOBJTYPE_DE_CLIENTINFO, m_ClientID, sizeof(CNetObj_De_ClientInfo)));
		if(!pClientInfo)
			return;

		pClientInfo->m_Local = 0;
		pClientInfo->m_Team = m_Team;
		StrToInts(pClientInfo->m_aName, 4, Server()->ClientName(m_ClientID));
		StrToInts(pClientInfo->m_aClan, 3, Server()->ClientClan(m_ClientID));
		pClientInfo->m_Country = Server()->ClientCountry(m_ClientID);

		for(int p = 0; p < NUM_SKINPARTS; p++)
		{
			StrToInts(pClientInfo->m_aaSkinPartNames[p], 6, m_TeeInfos.m_aaSkinPartNames[p]);
			pClientInfo->m_aUseCustomColors[p] = m_TeeInfos.m_aUseCustomColors[p];
			pClientInfo->m_aSkinPartColors[p] = m_TeeInfos.m_aSkinPartColors[p];
		}
	}
}

void CPlayer::OnDisconnect()
{
	KillCharacter();

	if(m_Team != TEAM_SPECTATORS)
	{
		// update spectator modes
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->m_SpecMode == SPEC_PLAYER && GameServer()->m_apPlayers[i]->m_SpectatorID == m_ClientID)
			{
				if(GameServer()->m_apPlayers[i]->m_DeadSpecMode)
					GameServer()->m_apPlayers[i]->UpdateDeadSpecMode();
				else
				{
					GameServer()->m_apPlayers[i]->m_SpecMode = SPEC_FREEVIEW;
					GameServer()->m_apPlayers[i]->m_SpectatorID = -1;
				}
			}
		}
	}
}

void CPlayer::OnPredictedInput(CNetObj_PlayerInput *NewInput)
{
	// skip the input if chat is active
	if((m_PlayerFlags&PLAYERFLAG_CHATTING) && (NewInput->m_PlayerFlags&PLAYERFLAG_CHATTING))
		return;

	if(m_pCharacter)
		m_pCharacter->OnPredictedInput(NewInput);
}

void CPlayer::OnDirectInput(CNetObj_PlayerInput *NewInput)
{
	if(GameServer()->m_World.m_Paused)
	{
		m_PlayerFlags = NewInput->m_PlayerFlags;
		return;
	}

	if(NewInput->m_PlayerFlags&PLAYERFLAG_CHATTING)
	{
		// skip the input if chat is active
		if(m_PlayerFlags&PLAYERFLAG_CHATTING)
			return;

		// reset input
		if(m_pCharacter)
			m_pCharacter->ResetInput();

		m_PlayerFlags = NewInput->m_PlayerFlags;
		return;
	}

	m_PlayerFlags = NewInput->m_PlayerFlags;

	if(m_pCharacter)
		m_pCharacter->OnDirectInput(NewInput);

	if(!m_pCharacter && m_Team != TEAM_SPECTATORS && (NewInput->m_Fire&1))
		Respawn();

	if(!m_pCharacter && m_Team == TEAM_SPECTATORS && (NewInput->m_Fire&1))
	{
		if(!m_ActiveSpecSwitch)
		{
			m_ActiveSpecSwitch = true;
			if(m_SpecMode == SPEC_FREEVIEW)
			{
				CCharacter *pChar = (CCharacter *)GameServer()->m_World.ClosestEntity(m_ViewPos, 6.0f*32, CGameWorld::ENTTYPE_CHARACTER, 0);
				CFlag *pFlag = (CFlag *)GameServer()->m_World.ClosestEntity(m_ViewPos, 6.0f*32, CGameWorld::ENTTYPE_FLAG, 0);
				if(pChar || pFlag)
				{
					if(!pChar || (pFlag && pChar && distance(m_ViewPos, pFlag->GetPos()) < distance(m_ViewPos, pChar->GetPos())))
					{
						m_SpecMode = pFlag->GetTeam() == TEAM_RED ? SPEC_FLAGRED : SPEC_FLAGBLUE;
						m_pSpecFlag = pFlag;
						m_SpectatorID = -1;
					}
					else
					{
						m_SpecMode = SPEC_PLAYER;
						m_pSpecFlag = 0;
						m_SpectatorID = pChar->GetPlayer()->GetCID();
					}
				}
			}
			else
			{
				m_SpecMode = SPEC_FREEVIEW;
				m_pSpecFlag = 0;
				m_SpectatorID = -1;
			}
		}
	}
	else if(m_ActiveSpecSwitch)
		m_ActiveSpecSwitch = false;

	// check for activity
	if(NewInput->m_Direction || m_LatestActivity.m_TargetX != NewInput->m_TargetX ||
		m_LatestActivity.m_TargetY != NewInput->m_TargetY || NewInput->m_Jump ||
		NewInput->m_Fire&1 || NewInput->m_Hook)
	{
		m_LatestActivity.m_TargetX = NewInput->m_TargetX;
		m_LatestActivity.m_TargetY = NewInput->m_TargetY;
		m_LastActionTick = Server()->Tick();
		m_InactivityTickCounter = 0;
	}
}

CCharacter *CPlayer::GetCharacter()
{
	if(m_pCharacter && m_pCharacter->IsAlive())
		return m_pCharacter;
	return 0;
}

void CPlayer::KillCharacter(int Weapon)
{
	if(m_pCharacter)
	{
		m_pCharacter->Die(m_ClientID, Weapon);
		delete m_pCharacter;
		m_pCharacter = 0;
	}
}

void CPlayer::Respawn()
{
	if(m_RespawnDisabled && m_Team != TEAM_SPECTATORS)
	{
		// enable spectate mode for dead players
		m_DeadSpecMode = true;
		m_IsReadyToPlay = true;
		m_SpecMode = SPEC_PLAYER;
		UpdateDeadSpecMode();
		return;
	}

	m_DeadSpecMode = false;

	if(m_Team != TEAM_SPECTATORS)
		m_Spawning = true;
}

bool CPlayer::SetSpectatorID(int SpecMode, int SpectatorID)
{
	if((SpecMode == m_SpecMode && SpecMode != SPEC_PLAYER) ||
		(m_SpecMode == SPEC_PLAYER && SpecMode == SPEC_PLAYER && (SpectatorID == -1 || m_SpectatorID == SpectatorID || m_ClientID == SpectatorID)))
	{
		return false;
	}

	if(m_Team == TEAM_SPECTATORS)
	{
		// check for freeview or if wanted player is playing
		if(SpecMode != SPEC_PLAYER || (SpecMode == SPEC_PLAYER && GameServer()->m_apPlayers[SpectatorID] && GameServer()->m_apPlayers[SpectatorID]->GetTeam() != TEAM_SPECTATORS))
		{
			if(SpecMode == SPEC_FLAGRED || SpecMode == SPEC_FLAGBLUE)
			{
				CFlag *pFlag = (CFlag*)GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_FLAG);
				while (pFlag)
				{
					if ((pFlag->GetTeam() == TEAM_RED && SpecMode == SPEC_FLAGRED) || (pFlag->GetTeam() == TEAM_BLUE && SpecMode == SPEC_FLAGBLUE))
					{
						m_pSpecFlag = pFlag;
						if (pFlag->GetCarrier())
							m_SpectatorID = pFlag->GetCarrier()->GetPlayer()->GetCID();
						else
							m_SpectatorID = -1;
						break;
					}
					pFlag = (CFlag*)pFlag->TypeNext();
				}
				if (!m_pSpecFlag)
					return false;
				m_SpecMode = SpecMode;
				return true;
			}
			m_pSpecFlag = 0;
			m_SpecMode = SpecMode;
			m_SpectatorID = SpectatorID;
			return true;
		}
	}
	else if(m_DeadSpecMode)
	{
		// check if wanted player can be followed
		if(SpecMode == SPEC_PLAYER && GameServer()->m_apPlayers[SpectatorID] && DeadCanFollow(GameServer()->m_apPlayers[SpectatorID]))
		{
			m_SpecMode = SpecMode;
			m_pSpecFlag = 0;
			m_SpectatorID = SpectatorID;
			return true;
		}
	}

	return false;
}

bool CPlayer::DeadCanFollow(CPlayer *pPlayer) const
{
	// check if wanted player is in the same team and alive
	return (!pPlayer->m_RespawnDisabled || (pPlayer->GetCharacter() && pPlayer->GetCharacter()->IsAlive())) && pPlayer->GetTeam() == m_Team;
}

void CPlayer::UpdateDeadSpecMode()
{
	// check if actual spectator id is valid
	if(m_SpectatorID != -1 && GameServer()->m_apPlayers[m_SpectatorID] && DeadCanFollow(GameServer()->m_apPlayers[m_SpectatorID]))
		return;

	// find player to follow
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(GameServer()->m_apPlayers[i] && DeadCanFollow(GameServer()->m_apPlayers[i]))
		{
			m_SpectatorID = i;
			return;
		}
	}

	// no one available to follow -> turn spectator mode off
	m_DeadSpecMode = false;
}

void CPlayer::SetTeam(int Team, bool DoChatMsg)
{
	KillCharacter();

	m_Team = Team;
	m_LastActionTick = Server()->Tick();
	m_SpecMode = SPEC_FREEVIEW;
	m_SpectatorID = -1;
	m_pSpecFlag = 0;
	m_DeadSpecMode = false;

	// we got to wait 0.5 secs before respawning
	m_RespawnTick = Server()->Tick()+Server()->TickSpeed()/2;

	if(Team == TEAM_SPECTATORS)
	{
		// update spectator modes
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(GameServer()->m_apPlayers[i] && GameServer()-> m_apPlayers[i]->m_SpecMode == SPEC_PLAYER && GameServer()->m_apPlayers[i]->m_SpectatorID == m_ClientID)
			{
				if(GameServer()->m_apPlayers[i]->m_DeadSpecMode)
					GameServer()->m_apPlayers[i]->UpdateDeadSpecMode();
				else
				{
					GameServer()->m_apPlayers[i]->m_SpecMode = SPEC_FREEVIEW;
					GameServer()->m_apPlayers[i]->m_SpectatorID = -1;
				}
			}
		}
	}
}

void CPlayer::TryRespawn()
{
	vec2 SpawnPos;

	if(!GameServer()->m_pController->CanSpawn(m_Team, &SpawnPos))
		return;

	m_Spawning = false;
	m_pCharacter = new(m_ClientID) CCharacter(&GameServer()->m_World);
	m_pCharacter->Spawn(this, SpawnPos);
	GameServer()->CreatePlayerSpawn(SpawnPos);
}

// solofng

void CPlayer::SetConfig(int Cfg)
{
	InitRoundStats();
	m_RoundStats.m_CfgFlags |= Cfg;
}

void CPlayer::UnsetConfig(int Cfg)
{
	InitRoundStats();
	m_RoundStats.m_CfgFlags &= ~(Cfg);
}

bool CPlayer::IsConfig(int Cfg)
{
	InitRoundStats();
	return m_RoundStats.m_CfgFlags&Cfg;
}

void CPlayer::AddKills(int Kills)
{
	InitRoundStats();
	m_RoundStats.m_Kills += Kills;
	m_RoundStats.m_LastKillTime = HandleMulti();
}

void CPlayer::AddDeaths(int Deaths)
{
	InitRoundStats();
	m_RoundStats.m_Deaths += Deaths;
}

void CPlayer::AddGoldSpikes(int Spikes)
{
	InitRoundStats();
	m_RoundStats.m_GoldSpikes += Spikes;
}

void CPlayer::AddGreenSpikes(int Spikes)
{
	InitRoundStats();
	m_RoundStats.m_GreenSpikes += Spikes;
}

void CPlayer::AddPurpleSpikes(int Spikes)
{
	InitRoundStats();
	m_RoundStats.m_PurpleSpikes += Spikes;
}

void CPlayer::AddShots(int Shots)
{
	InitRoundStats();
	m_RoundStats.m_RifleShots += Shots;
}

void CPlayer::HandleSpreeDeath(const char *pKiller)
{
	if (m_RoundStats.m_Spree >= 5) {
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "'%s' spree of %d kills ended by '%s'!",
			Server()->ClientName(m_ClientID), m_RoundStats.m_Spree, pKiller);
		GameServer()->SendChat(-1, CHAT_ALL, -1, aBuf);
	}
	if (m_RoundStats.m_Spree > m_RoundStats.m_SpreeBest)
		m_RoundStats.m_SpreeBest = m_RoundStats.m_Spree;
	m_RoundStats.m_Spree = 0;
}

void CPlayer::HandleSpreeKill()
{
	if (((++m_RoundStats.m_Spree) % 5) == 0) {
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "'%s' is on a spree of %d kills!",
			Server()->ClientName(m_ClientID), m_RoundStats.m_Spree);
		GameServer()->SendChat(-1, CHAT_ALL, -1, aBuf);
	}
}

time_t CPlayer::HandleMulti()
{
	InitRoundStats();
	// full credits go to onbgy
	// https://github.com/nobody-mb/teeworlds-fng2-mod/blob/master/src/game/server/stats.cpp#L685
	time_t ttmp = time(NULL);
	if ((ttmp - m_RoundStats.m_LastKillTime) > 5)
	{
		m_RoundStats.m_Multi = 1;
		return ttmp;
	}
	m_RoundStats.m_Multi++;
	if (m_RoundStats.m_MultiBest < m_RoundStats.m_Multi)
		m_RoundStats.m_MultiBest = m_RoundStats.m_Multi;
	int index = m_RoundStats.m_Multi - 2;
	m_RoundStats.m_aMultis[index > MAX_MULTIS ? MAX_MULTIS : index]++;
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "'%s' multi x%d!",
		Server()->ClientName(m_ClientID), m_RoundStats.m_Multi);
	GameServer()->SendChat(-1, CHAT_ALL, -1, aBuf);
	return ttmp;
}

void CPlayer::AddFreezes(int Freezes)
{
	InitRoundStats();
	m_RoundStats.m_Freezes += Freezes;
}

void CPlayer::AddFrozen(int Frozen)
{
	InitRoundStats();
	m_RoundStats.m_Frozen += Frozen;
}

void CPlayer::InitRoundStats()
{
	if (m_InitedRoundStats)
		return;
	m_InitedRoundStats = true;
	CFngStats Stats;
	bool HasStats = true;
	if (GameServer()->LoadStats(-1, Server()->ClientName(m_ClientID), &Stats) != 0)
		HasStats = false;
	dbg_msg("stats", "init round stats ClientID=%d HasStats=%d", m_ClientID, HasStats);
	// mem_zero probably redundants all the explicit 0 intializations but what ever
	mem_zero(&m_RoundStats, sizeof(m_RoundStats));
	str_copy(m_RoundStats.m_aName, Server()->ClientName(m_ClientID), sizeof(m_RoundStats.m_aName));
	str_copy(m_RoundStats.m_aClan, Server()->ClientClan(m_ClientID), sizeof(m_RoundStats.m_aClan));
	m_RoundStats.m_Kills = 0;
	m_RoundStats.m_Deaths = 0;
	m_RoundStats.m_GoldSpikes = 0;
	m_RoundStats.m_GreenSpikes = 0;
	m_RoundStats.m_PurpleSpikes = 0;
	m_RoundStats.m_RifleShots = 0;
	m_RoundStats.m_Freezes = 0;
	m_RoundStats.m_Frozen = 0;
	m_RoundStats.m_Spree = 0;
	m_RoundStats.m_SpreeBest = 0;
	m_RoundStats.m_LastKillTime = 0;
	m_RoundStats.m_Multi = 0;
	m_RoundStats.m_MultiBest = 0;
	for (int i = 0; i < MAX_MULTIS; i++)
	{
		m_RoundStats.m_aMultis[i] = 0;
	}
	m_RoundStats.m_Tmp = 0;
	m_RoundStats.m_CfgFlags = 0;
	if (HasStats)
		m_RoundStats.m_CfgFlags = Stats.m_CfgFlags;
	m_RoundStats.m_Unused2 = 0;
	m_RoundStats.m_FirstSeen = 0;
	m_RoundStats.m_LastSeen = time(NULL);
}

bool CPlayer::SaveStats(const char *pFilePath)
{
	InitRoundStats();
	// 'foo's killingspree was ended by 'foo'
	// disconnect, round end etc is basically a selfkill
	HandleSpreeDeath(Server()->ClientName(m_ClientID));
	CFngStats *pMergeStats;
	CFngStats FileStats;
	m_RoundStats.m_TotalOnlineTime = time(NULL) - m_JoinTime;
	bool HasStast = GameServer()->LoadStats(m_ClientID, Server()->ClientName(m_ClientID), &FileStats) == 0;
	if (!HasStast)
	{
		// just write round stats to file
		m_RoundStats.m_FirstSeen = time(NULL);
		pMergeStats = &m_RoundStats;
	}
	else
	{
		GameServer()->MergeStats(&m_RoundStats, &FileStats);
		pMergeStats = &FileStats;
	}

	FILE *pFile;
#if defined(CONF_FAMILY_UNIX)
	int fd;
	struct stat st0, st1;
	pFile = fopen(pFilePath, "wb");
	char aLockPath[2048+4];
	str_format(aLockPath, sizeof(aLockPath), "%s.lck", pFilePath);
	// lock file code by user2769258 and Arnaud Le Blanc
	// https://stackoverflow.com/a/18745264
	// Not portable! E.g. on Windows st_ino is always 0. – rustyx Nov 8 '17 at 18:35
	// Windows has own lock system (note by ChillerDragon)
	while(1)
	{
		fd = open(aLockPath, O_CREAT, S_IRUSR|S_IWUSR);
		flock(fd, LOCK_EX);

		fstat(fd, &st0);
		stat(aLockPath, &st1);
		if(st0.st_ino == st1.st_ino) break;

		dbg_msg("stats", "wait for locked file...");
		close(fd);
	}
#endif
	if(!pFile)
	{
		GameServer()->SendChatTarget(m_ClientID, "[stats] save failed: file open.");
		return false;
	}
	fwrite(&FNG_MAGIC, sizeof(FNG_MAGIC), 1, pFile);
	fwrite(&FNG_VERSION, sizeof(FNG_VERSION), 1, pFile);
	fwrite(pMergeStats, sizeof(*pMergeStats), 1, pFile);
	fclose(pFile);
#if defined(CONF_FAMILY_UNIX)
	unlink(aLockPath);
	flock(fd, LOCK_UN);
#endif
	dbg_msg("stats", "saved ClientID=%d to file '%s' (%s)", m_ClientID, pFilePath, HasStast ? "merge" : "new");
	m_InitedRoundStats = false;
	InitRoundStats(); // Refresh/Delete round stats
	return true;
}
