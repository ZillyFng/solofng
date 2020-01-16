/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <stdio.h>
#include <dirent.h>
#include <base/math.h>
#include <vector>
#include <algorithm>

#include <engine/shared/config.h>
#include <engine/shared/memheap.h>
#include <engine/map.h>

#include <generated/server_data.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/version.h>

#if defined(CONF_FAMILY_UNIX)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#include "entities/character.h"
#include "entities/projectile.h"
#include "gamemodes/ctf.h"
#include "gamemodes/dm.h"
#include "gamemodes/lms.h"
#include "gamemodes/lts.h"
#include "gamemodes/mod.h"
#include "gamemodes/tdm.h"
#include "gamecontext.h"
#include "player.h"

enum
{
	RESET,
	NO_RESET
};

void CGameContext::Construct(int Resetting)
{
	m_Resetting = 0;
	m_pServer = 0;

	for(int i = 0; i < MAX_CLIENTS; i++)
		m_apPlayers[i] = 0;

	m_pController = 0;
	m_VoteCloseTime = 0;
	m_VoteCancelTime = 0;
	m_pVoteOptionFirst = 0;
	m_pVoteOptionLast = 0;
	m_NumVoteOptions = 0;
	m_LockTeams = 0;

	if(Resetting==NO_RESET)
		m_pVoteOptionHeap = new CHeap();

	// solofng

	m_StatSaveFails = 0;
	m_StatSaveCriticalFails = 0;
}

CGameContext::CGameContext(int Resetting)
{
	Construct(Resetting);
}

CGameContext::CGameContext()
{
	Construct(NO_RESET);
}

CGameContext::~CGameContext()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
		delete m_apPlayers[i];
	if(!m_Resetting)
		delete m_pVoteOptionHeap;
}

void CGameContext::Clear()
{
	CHeap *pVoteOptionHeap = m_pVoteOptionHeap;
	CVoteOptionServer *pVoteOptionFirst = m_pVoteOptionFirst;
	CVoteOptionServer *pVoteOptionLast = m_pVoteOptionLast;
	int NumVoteOptions = m_NumVoteOptions;
	CTuningParams Tuning = m_Tuning;

	m_Resetting = true;
	this->~CGameContext();
	mem_zero(this, sizeof(*this));
	new (this) CGameContext(RESET);

	m_pVoteOptionHeap = pVoteOptionHeap;
	m_pVoteOptionFirst = pVoteOptionFirst;
	m_pVoteOptionLast = pVoteOptionLast;
	m_NumVoteOptions = NumVoteOptions;
	m_Tuning = Tuning;
}


class CCharacter *CGameContext::GetPlayerChar(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !m_apPlayers[ClientID])
		return 0;
	return m_apPlayers[ClientID]->GetCharacter();
}

void CGameContext::CreateDamage(vec2 Pos, int Id, vec2 Source, int HealthAmount, int ArmorAmount, bool Self, int FromPlayerID)
{
	float f = angle(Source);
	int64 mask = -1;
	if(FromPlayerID != -1)
	{
		mask = 0;
		mask |= CmaskOne(Id);
	}
	CNetEvent_Damage *pEvent = (CNetEvent_Damage *)m_Events.Create(NETEVENTTYPE_DAMAGE, sizeof(CNetEvent_Damage), mask);
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_ClientID = Id;
		pEvent->m_Angle = (int)(f*256.0f);
		pEvent->m_HealthAmount = HealthAmount;
		pEvent->m_ArmorAmount = ArmorAmount;
		pEvent->m_Self = Self;
	}
}

void CGameContext::CreateHammerHit(vec2 Pos)
{
	// create the event
	CNetEvent_HammerHit *pEvent = (CNetEvent_HammerHit *)m_Events.Create(NETEVENTTYPE_HAMMERHIT, sizeof(CNetEvent_HammerHit));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}
}


void CGameContext::CreateExplosion(vec2 Pos, int Owner, int Weapon, int MaxDamage)
{
	// create the event
	CNetEvent_Explosion *pEvent = (CNetEvent_Explosion *)m_Events.Create(NETEVENTTYPE_EXPLOSION, sizeof(CNetEvent_Explosion));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}

	// deal damage
	CCharacter *apEnts[MAX_CLIENTS];
	float Radius = g_pData->m_Explosion.m_Radius;
	float InnerRadius = 48.0f;
	float MaxForce = g_pData->m_Explosion.m_MaxForce;
	int Num = m_World.FindEntities(Pos, Radius, (CEntity**)apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
	for(int i = 0; i < Num; i++)
	{
		vec2 Diff = apEnts[i]->GetPos() - Pos;
		vec2 Force(0, MaxForce);
		float l = length(Diff);
		if(l)
			Force = normalize(Diff) * MaxForce;
		float Factor = 1 - clamp((l-InnerRadius)/(Radius-InnerRadius), 0.0f, 1.0f);
		if((int)(Factor * MaxDamage))
			apEnts[i]->TakeDamage(Force * Factor, Diff*-1, (int)(Factor * MaxDamage), Owner, Weapon);
	}
}

void CGameContext::CreatePlayerSpawn(vec2 Pos)
{
	// create the event
	CNetEvent_Spawn *ev = (CNetEvent_Spawn *)m_Events.Create(NETEVENTTYPE_SPAWN, sizeof(CNetEvent_Spawn));
	if(ev)
	{
		ev->m_X = (int)Pos.x;
		ev->m_Y = (int)Pos.y;
	}
}

void CGameContext::CreateDeath(vec2 Pos, int ClientID)
{
	// create the event
	CNetEvent_Death *pEvent = (CNetEvent_Death *)m_Events.Create(NETEVENTTYPE_DEATH, sizeof(CNetEvent_Death));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_ClientID = ClientID;
	}
}

void CGameContext::CreateSound(vec2 Pos, int Sound, int64 Mask)
{
	if (Sound < 0)
		return;

	// create a sound
	CNetEvent_SoundWorld *pEvent = (CNetEvent_SoundWorld *)m_Events.Create(NETEVENTTYPE_SOUNDWORLD, sizeof(CNetEvent_SoundWorld), Mask);
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_SoundID = Sound;
	}
}

void CGameContext::SendChatTarget(int To, const char *pText)
{
	CNetMsg_Sv_Chat Msg;
	Msg.m_Mode = CHAT_ALL;
	Msg.m_ClientID = -1;
	Msg.m_TargetID = -1; // use this to get ugly whisper
	Msg.m_pMessage = pText;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, To);
}

void CGameContext::SendChat(int ChatterClientID, int Mode, int To, const char *pText)
{
	char aBuf[256];
	if(ChatterClientID >= 0 && ChatterClientID < MAX_CLIENTS)
		str_format(aBuf, sizeof(aBuf), "%d:%d:%s: %s", ChatterClientID, Mode, Server()->ClientName(ChatterClientID), pText);
	else
		str_format(aBuf, sizeof(aBuf), "*** %s", pText);

	char aBufMode[32];
	if(Mode == CHAT_WHISPER)
		str_copy(aBufMode, "whisper", sizeof(aBufMode));
	else if(Mode == CHAT_TEAM)
		str_copy(aBufMode, "teamchat", sizeof(aBufMode));
	else
		str_copy(aBufMode, "chat", sizeof(aBufMode));

	Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, aBufMode, aBuf);


	CNetMsg_Sv_Chat Msg;
	Msg.m_Mode = Mode;
	Msg.m_ClientID = ChatterClientID;
	Msg.m_pMessage = pText;
	Msg.m_TargetID = -1;

	if(Mode == CHAT_ALL)
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
	else if(Mode == CHAT_TEAM)
	{
		// pack one for the recording only
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NOSEND, -1);

		To = m_apPlayers[ChatterClientID]->GetTeam();

		// send to the clients
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(m_apPlayers[i] && m_apPlayers[i]->GetTeam() == To)
				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NORECORD, i);
		}
	}
	else // Mode == CHAT_WHISPER
	{
		// send to the clients
		Msg.m_TargetID = To;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ChatterClientID);
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, To);
	}
}

void CGameContext::SendBroadcast(const char* pText, int ClientID)
{
	CNetMsg_Sv_Broadcast Msg;
	Msg.m_pMessage = pText;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendEmoticon(int ClientID, int Emoticon)
{
	CNetMsg_Sv_Emoticon Msg;
	Msg.m_ClientID = ClientID;
	Msg.m_Emoticon = Emoticon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CGameContext::SendWeaponPickup(int ClientID, int Weapon)
{
	CNetMsg_Sv_WeaponPickup Msg;
	Msg.m_Weapon = Weapon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendMotd(int ClientID)
{
	CNetMsg_Sv_Motd Msg;
	Msg.m_pMessage = g_Config.m_SvMotd;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendSettings(int ClientID)
{
	CNetMsg_Sv_ServerSettings Msg;
	Msg.m_KickVote = g_Config.m_SvVoteKick;
	Msg.m_KickMin = g_Config.m_SvVoteKickMin;
	Msg.m_SpecVote = g_Config.m_SvVoteSpectate;
	Msg.m_TeamLock = m_LockTeams != 0;
	Msg.m_TeamBalance = g_Config.m_SvTeambalanceTime != 0;
	Msg.m_PlayerSlots = g_Config.m_SvPlayerSlots;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendSkinChange(int ClientID, int TargetID)
{
	CNetMsg_Sv_SkinChange Msg;
	Msg.m_ClientID = ClientID;
	for(int p = 0; p < NUM_SKINPARTS; p++)
	{
		Msg.m_apSkinPartNames[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aaSkinPartNames[p];
		Msg.m_aUseCustomColors[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aUseCustomColors[p];
		Msg.m_aSkinPartColors[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aSkinPartColors[p];
	}
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NORECORD, TargetID);
}

void CGameContext::SendGameMsg(int GameMsgID, int ClientID)
{
	CMsgPacker Msg(NETMSGTYPE_SV_GAMEMSG);
	Msg.AddInt(GameMsgID);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendGameMsg(int GameMsgID, int ParaI1, int ClientID)
{
	CMsgPacker Msg(NETMSGTYPE_SV_GAMEMSG);
	Msg.AddInt(GameMsgID);
	Msg.AddInt(ParaI1);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendGameMsg(int GameMsgID, int ParaI1, int ParaI2, int ParaI3, int ClientID)
{
	CMsgPacker Msg(NETMSGTYPE_SV_GAMEMSG);
	Msg.AddInt(GameMsgID);
	Msg.AddInt(ParaI1);
	Msg.AddInt(ParaI2);
	Msg.AddInt(ParaI3);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

//
void CGameContext::StartVote(const char *pDesc, const char *pCommand, const char *pReason)
{
	// check if a vote is already running
	if(m_VoteCloseTime)
		return;

	// reset votes
	m_VoteEnforce = VOTE_ENFORCE_UNKNOWN;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			m_apPlayers[i]->m_Vote = 0;
			m_apPlayers[i]->m_VotePos = 0;
		}
	}

	// start vote
	m_VoteCloseTime = time_get() + time_freq()*VOTE_TIME;
	m_VoteCancelTime = time_get() + time_freq()*VOTE_CANCEL_TIME;
	str_copy(m_aVoteDescription, pDesc, sizeof(m_aVoteDescription));
	str_copy(m_aVoteCommand, pCommand, sizeof(m_aVoteCommand));
	str_copy(m_aVoteReason, pReason, sizeof(m_aVoteReason));
	SendVoteSet(m_VoteType, -1);
	m_VoteUpdate = true;
}


void CGameContext::EndVote(int Type, bool Force)
{
	m_VoteCloseTime = 0;
	m_VoteCancelTime = 0;
	if(Force)
		m_VoteCreator = -1;
	SendVoteSet(Type, -1);
}

void CGameContext::ForceVote(int Type, const char *pDescription, const char *pReason)
{
	CNetMsg_Sv_VoteSet Msg;
	Msg.m_Type = Type;
	Msg.m_Timeout = 0;
	Msg.m_ClientID = -1;
	Msg.m_pDescription = pDescription;
	Msg.m_pReason = pReason;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CGameContext::SendVoteSet(int Type, int ToClientID)
{
	CNetMsg_Sv_VoteSet Msg;
	if(m_VoteCloseTime)
	{
		Msg.m_ClientID = m_VoteCreator;
		Msg.m_Type = Type;
		Msg.m_Timeout = (m_VoteCloseTime-time_get())/time_freq();
		Msg.m_pDescription = m_aVoteDescription;
		Msg.m_pReason = m_aVoteReason;
	}
	else
	{
		Msg.m_Type = Type;
		Msg.m_Timeout = 0;
		Msg.m_ClientID = m_VoteCreator;
		Msg.m_pDescription = "";
		Msg.m_pReason = "";
	}
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ToClientID);
}

void CGameContext::SendVoteStatus(int ClientID, int Total, int Yes, int No)
{
	CNetMsg_Sv_VoteStatus Msg = {0};
	Msg.m_Total = Total;
	Msg.m_Yes = Yes;
	Msg.m_No = No;
	Msg.m_Pass = Total - (Yes+No);

	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);

}

void CGameContext::AbortVoteOnDisconnect(int ClientID)
{
	if(m_VoteCloseTime && ClientID == m_VoteClientID && (str_startswith(m_aVoteCommand, "kick ") ||
		str_startswith(m_aVoteCommand, "set_team ") || (str_startswith(m_aVoteCommand, "ban ") && Server()->IsBanned(ClientID))))
		m_VoteCloseTime = -1;
}

void CGameContext::AbortVoteOnTeamChange(int ClientID)
{
	if(m_VoteCloseTime && ClientID == m_VoteClientID && str_startswith(m_aVoteCommand, "set_team "))
		m_VoteCloseTime = -1;
}


void CGameContext::CheckPureTuning()
{
	// might not be created yet during start up
	if(!m_pController)
		return;

	if(	str_comp(m_pController->GetGameType(), "DM")==0 ||
		str_comp(m_pController->GetGameType(), "TDM")==0 ||
		str_comp(m_pController->GetGameType(), "CTF")==0 ||
		str_comp(m_pController->GetGameType(), "LMS")==0 ||
		str_comp(m_pController->GetGameType(), "LTS")==0)
	{
		CTuningParams p;
		if(mem_comp(&p, &m_Tuning, sizeof(p)) != 0)
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "resetting tuning due to pure server");
			m_Tuning = p;
		}
	}
}

void CGameContext::SendTuningParams(int ClientID)
{
	CheckPureTuning();

	CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
	int *pParams = (int *)&m_Tuning;
	for(unsigned i = 0; i < sizeof(m_Tuning)/sizeof(int); i++)
		Msg.AddInt(pParams[i]);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SwapTeams()
{
	if(!m_pController->IsTeamplay())
		return;

	SendGameMsg(GAMEMSG_TEAM_SWAP, -1);

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(m_apPlayers[i] && m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
			m_pController->DoTeamChange(m_apPlayers[i], m_apPlayers[i]->GetTeam()^1, false);
	}

	m_pController->SwapTeamscore();
}

void CGameContext::RankThreadTick()
{
	if (m_RankThreadState == RT_IDLE)
		return;
	if (m_RankThreadState == RT_DONE)
	{
		if (m_RankThreadType == TYPE_RANK)
		{
			SendChat(-1, CHAT_ALL, -1, m_aRankThreadResult[0]);
		}
		else // /top5
		{
			SendChatTarget(m_RankThreadReqID, "----------- Top 5 -----------");
			for (int i = 0; i < 5; i++)
				SendChatTarget(m_RankThreadReqID, m_aRankThreadResult[i]);
			SendChatTarget(m_RankThreadReqID, "-------------------------------");
		}
	}
	else if (m_RankThreadState == RT_ERR)
	{
		SendChatTarget(m_RankThreadReqID, m_aRankThreadResult[0]);
	}
	dbg_msg(
		"solofng",
		"finished %s thread. (%s)",
		m_RankThreadType == TYPE_RANK ? "rank" : "top",
		m_RankThreadState == RT_DONE ? "success" : "error"
	);
	m_RankThreadState = RT_IDLE;
}

void CGameContext::SolofngTick()
{
	RankThreadTick();
}

void CGameContext::OnTick()
{
	SolofngTick();

	// check tuning
	CheckPureTuning();

	// copy tuning
	m_World.m_Core.m_Tuning = m_Tuning;
	m_World.Tick();

	//if(world.paused) // make sure that the game object always updates
	m_pController->Tick();

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			m_apPlayers[i]->Tick();
			m_apPlayers[i]->PostTick();
		}
	}

	// update voting
	if(m_VoteCloseTime)
	{
		// abort the kick-vote on player-leave
		if(m_VoteCloseTime == -1)
			EndVote(VOTE_END_ABORT, false);
		else
		{
			int Total = 0, Yes = 0, No = 0;
			if(m_VoteUpdate)
			{
				// count votes
				char aaBuf[MAX_CLIENTS][NETADDR_MAXSTRSIZE] = {{0}};
				for(int i = 0; i < MAX_CLIENTS; i++)
					if(m_apPlayers[i])
						Server()->GetClientAddr(i, aaBuf[i], NETADDR_MAXSTRSIZE);
				bool aVoteChecked[MAX_CLIENTS] = {0};
				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(!m_apPlayers[i] || m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS || aVoteChecked[i])	// don't count in votes by spectators
						continue;

					int ActVote = m_apPlayers[i]->m_Vote;
					int ActVotePos = m_apPlayers[i]->m_VotePos;

					// check for more players with the same ip (only use the vote of the one who voted first)
					for(int j = i+1; j < MAX_CLIENTS; ++j)
					{
						if(!m_apPlayers[j] || aVoteChecked[j] || str_comp(aaBuf[j], aaBuf[i]))
							continue;

						aVoteChecked[j] = true;
						if(m_apPlayers[j]->m_Vote && (!ActVote || ActVotePos > m_apPlayers[j]->m_VotePos))
						{
							ActVote = m_apPlayers[j]->m_Vote;
							ActVotePos = m_apPlayers[j]->m_VotePos;
						}
					}

					Total++;
					if(ActVote > 0)
						Yes++;
					else if(ActVote < 0)
						No++;
				}
			}

			if(m_VoteEnforce == VOTE_ENFORCE_YES || (m_VoteUpdate && Yes >= Total/2+1))
			{
				Server()->SetRconCID(IServer::RCON_CID_VOTE);
				Console()->ExecuteLine(m_aVoteCommand);
				Server()->SetRconCID(IServer::RCON_CID_SERV);
				if(m_VoteCreator != -1 && m_apPlayers[m_VoteCreator])
					m_apPlayers[m_VoteCreator]->m_LastVoteCall = 0;

				EndVote(VOTE_END_PASS, m_VoteEnforce==VOTE_ENFORCE_YES);
			}
			else if(m_VoteEnforce == VOTE_ENFORCE_NO || (m_VoteUpdate && No >= (Total+1)/2) || time_get() > m_VoteCloseTime)
				EndVote(VOTE_END_FAIL, m_VoteEnforce==VOTE_ENFORCE_NO);
			else if(m_VoteUpdate)
			{
				m_VoteUpdate = false;
				SendVoteStatus(-1, Total, Yes, No);
			}
		}
	}


#ifdef CONF_DEBUG
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i] && m_apPlayers[i]->IsDummy())
		{
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = (i&1)?-1:1;
			m_apPlayers[i]->OnPredictedInput(&Input);
		}
	}
#endif
}

// Server hooks
void CGameContext::OnClientDirectInput(int ClientID, void *pInput)
{
	int NumFailures = m_NetObjHandler.NumObjFailures();
	if(m_NetObjHandler.ValidateObj(NETOBJTYPE_PLAYERINPUT, pInput, sizeof(CNetObj_PlayerInput)) == -1)
	{
		if(g_Config.m_Debug && NumFailures != m_NetObjHandler.NumObjFailures())
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "NETOBJTYPE_PLAYERINPUT failed on '%s'", m_NetObjHandler.FailedObjOn());
			Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
		}
	}
	else
		m_apPlayers[ClientID]->OnDirectInput((CNetObj_PlayerInput *)pInput);
}

void CGameContext::OnClientPredictedInput(int ClientID, void *pInput)
{
	if(!m_World.m_Paused)
	{
		int NumFailures = m_NetObjHandler.NumObjFailures();
		if(m_NetObjHandler.ValidateObj(NETOBJTYPE_PLAYERINPUT, pInput, sizeof(CNetObj_PlayerInput)) == -1)
		{
			if(g_Config.m_Debug && NumFailures != m_NetObjHandler.NumObjFailures())
			{
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf), "NETOBJTYPE_PLAYERINPUT corrected on '%s'", m_NetObjHandler.FailedObjOn());
				Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
			}
		}
		else
			m_apPlayers[ClientID]->OnPredictedInput((CNetObj_PlayerInput *)pInput);
	}
}

void CGameContext::OnClientEnter(int ClientID)
{
	m_pController->OnPlayerConnect(m_apPlayers[ClientID]);

	m_VoteUpdate = true;

	// update client infos (others before local)
	CNetMsg_Sv_ClientInfo NewClientInfoMsg;
	NewClientInfoMsg.m_ClientID = ClientID;
	NewClientInfoMsg.m_Local = 0;
	NewClientInfoMsg.m_Team = m_apPlayers[ClientID]->GetTeam();
	NewClientInfoMsg.m_pName = Server()->ClientName(ClientID);
	NewClientInfoMsg.m_pClan = Server()->ClientClan(ClientID);
	NewClientInfoMsg.m_Country = Server()->ClientCountry(ClientID);
	NewClientInfoMsg.m_Silent = false;

	if(g_Config.m_SvSilentSpectatorMode && m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS)
		NewClientInfoMsg.m_Silent = true;

	for(int p = 0; p < NUM_SKINPARTS; p++)
	{
		NewClientInfoMsg.m_apSkinPartNames[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aaSkinPartNames[p];
		NewClientInfoMsg.m_aUseCustomColors[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aUseCustomColors[p];
		NewClientInfoMsg.m_aSkinPartColors[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aSkinPartColors[p];
	}


	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(i == ClientID || !m_apPlayers[i] || (!Server()->ClientIngame(i) && !m_apPlayers[i]->IsDummy()))
			continue;

		// new info for others
		if(Server()->ClientIngame(i))
			Server()->SendPackMsg(&NewClientInfoMsg, MSGFLAG_VITAL|MSGFLAG_NORECORD, i);

		// existing infos for new player
		CNetMsg_Sv_ClientInfo ClientInfoMsg;
		ClientInfoMsg.m_ClientID = i;
		ClientInfoMsg.m_Local = 0;
		ClientInfoMsg.m_Team = m_apPlayers[i]->GetTeam();
		ClientInfoMsg.m_pName = Server()->ClientName(i);
		ClientInfoMsg.m_pClan = Server()->ClientClan(i);
		ClientInfoMsg.m_Country = Server()->ClientCountry(i);
		ClientInfoMsg.m_Silent = false;
		for(int p = 0; p < NUM_SKINPARTS; p++)
		{
			ClientInfoMsg.m_apSkinPartNames[p] = m_apPlayers[i]->m_TeeInfos.m_aaSkinPartNames[p];
			ClientInfoMsg.m_aUseCustomColors[p] = m_apPlayers[i]->m_TeeInfos.m_aUseCustomColors[p];
			ClientInfoMsg.m_aSkinPartColors[p] = m_apPlayers[i]->m_TeeInfos.m_aSkinPartColors[p];
		}
		Server()->SendPackMsg(&ClientInfoMsg, MSGFLAG_VITAL|MSGFLAG_NORECORD, ClientID);
	}

	// local info
	NewClientInfoMsg.m_Local = 1;
	Server()->SendPackMsg(&NewClientInfoMsg, MSGFLAG_VITAL|MSGFLAG_NORECORD, ClientID);

	if(Server()->DemoRecorder_IsRecording())
	{
		CNetMsg_De_ClientEnter Msg;
		Msg.m_pName = NewClientInfoMsg.m_pName;
		Msg.m_ClientID = ClientID;
		Msg.m_Team = NewClientInfoMsg.m_Team;
		Server()->SendPackMsg(&Msg, MSGFLAG_NOSEND, -1);
	}
}

void CGameContext::OnClientConnected(int ClientID, bool Dummy, bool AsSpec)
{
	if(m_apPlayers[ClientID])
	{
		dbg_assert(m_apPlayers[ClientID]->IsDummy(), "invalid clientID");
		OnClientDrop(ClientID, "removing dummy");
	}

	m_apPlayers[ClientID] = new(ClientID) CPlayer(this, ClientID, Dummy, AsSpec);

	if(Dummy)
		return;

	// send active vote
	if(m_VoteCloseTime)
		SendVoteSet(m_VoteType, ClientID);

	// send motd
	SendMotd(ClientID);

	// send settings
	SendSettings(ClientID);
}

void CGameContext::OnClientTeamChange(int ClientID)
{
	if(m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS)
		AbortVoteOnTeamChange(ClientID);

	// mark client's projectile has team projectile
	CProjectile *p = (CProjectile *)m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE);
	for(; p; p = (CProjectile *)p->TypeNext())
	{
		if(p->GetOwner() == ClientID)
			p->LoseOwner();
	}
}

void CGameContext::OnClientDrop(int ClientID, const char *pReason)
{
	SaveStats(ClientID);
	AbortVoteOnDisconnect(ClientID);
	m_pController->OnPlayerDisconnect(m_apPlayers[ClientID]);

	// update clients on drop
	if(Server()->ClientIngame(ClientID))
	{
		if(Server()->DemoRecorder_IsRecording())
		{
			CNetMsg_De_ClientLeave Msg;
			Msg.m_pName = Server()->ClientName(ClientID);
			Msg.m_pReason = pReason;
			Server()->SendPackMsg(&Msg, MSGFLAG_NOSEND, -1);
		}

		CNetMsg_Sv_ClientDrop Msg;
		Msg.m_ClientID = ClientID;
		Msg.m_pReason = pReason;
		Msg.m_Silent = false;
		if(g_Config.m_SvSilentSpectatorMode && m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS)
			Msg.m_Silent = true;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NORECORD, -1);
	}

	// mark client's projectile has team projectile
	CProjectile *p = (CProjectile *)m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE);
	for(; p; p = (CProjectile *)p->TypeNext())
	{
		if(p->GetOwner() == ClientID)
			p->LoseOwner();
	}

	delete m_apPlayers[ClientID];
	m_apPlayers[ClientID] = 0;

	m_VoteUpdate = true;
}

void CGameContext::OnMessage(int MsgID, CUnpacker *pUnpacker, int ClientID)
{
	void *pRawMsg = m_NetObjHandler.SecureUnpackMsg(MsgID, pUnpacker);
	CPlayer *pPlayer = m_apPlayers[ClientID];

	if(!pRawMsg)
	{
		if(g_Config.m_Debug)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "dropped weird message '%s' (%d), failed on '%s'", m_NetObjHandler.GetMsgName(MsgID), MsgID, m_NetObjHandler.FailedMsgOn());
			Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
		}
		return;
	}

	if(Server()->ClientIngame(ClientID))
	{
		if(MsgID == NETMSGTYPE_CL_SAY)
		{
			if(g_Config.m_SvSpamprotection && pPlayer->m_LastChat && pPlayer->m_LastChat+Server()->TickSpeed() > Server()->Tick())
				return;

			CNetMsg_Cl_Say *pMsg = (CNetMsg_Cl_Say *)pRawMsg;

			// trim right and set maximum length to 128 utf8-characters
			int Length = 0;
			const char *p = pMsg->m_pMessage;
			const char *pEnd = 0;
			while(*p)
			{
				const char *pStrOld = p;
				int Code = str_utf8_decode(&p);

				// check if unicode is not empty
				if(!str_utf8_is_whitespace(Code))
				{
					pEnd = 0;
				}
				else if(pEnd == 0)
					pEnd = pStrOld;

				if(++Length >= 127)
				{
					*(const_cast<char *>(p)) = 0;
					break;
				}
			}
			if(pEnd != 0)
				*(const_cast<char *>(pEnd)) = 0;

			// drop empty and autocreated spam messages (more than 20 characters per second)
			if(Length == 0 || (g_Config.m_SvSpamprotection && pPlayer->m_LastChat && pPlayer->m_LastChat + Server()->TickSpeed()*(Length/20) > Server()->Tick()))
				return;

			pPlayer->m_LastChat = Server()->Tick();

			// don't allow spectators to disturb players during a running game in tournament mode
			int Mode = pMsg->m_Mode;
			if((g_Config.m_SvTournamentMode == 2) &&
				pPlayer->GetTeam() == TEAM_SPECTATORS &&
				m_pController->IsGameRunning() &&
				!Server()->IsAuthed(ClientID))
			{
				if(Mode != CHAT_WHISPER)
					Mode = CHAT_TEAM;
				else if(m_apPlayers[pMsg->m_Target] && m_apPlayers[pMsg->m_Target]->GetTeam() != TEAM_SPECTATORS)
					Mode = CHAT_NONE;
			}

			if(pMsg->m_pMessage[0] == '/')
				ChatCommand(ClientID, &pMsg->m_pMessage[1]);
			else if(Mode != CHAT_NONE)
				SendChat(ClientID, Mode, pMsg->m_Target, pMsg->m_pMessage);
		}
		else if(MsgID == NETMSGTYPE_CL_CALLVOTE)
		{
			CNetMsg_Cl_CallVote *pMsg = (CNetMsg_Cl_CallVote *)pRawMsg;
			int64 Now = Server()->Tick();

			if(pMsg->m_Force)
			{
				if(!Server()->IsAuthed(ClientID))
					return;
			}
			else
			{
				if((g_Config.m_SvSpamprotection && ((pPlayer->m_LastVoteTry && pPlayer->m_LastVoteTry+Server()->TickSpeed()*3 > Now) ||
					(pPlayer->m_LastVoteCall && pPlayer->m_LastVoteCall+Server()->TickSpeed()*VOTE_COOLDOWN > Now))) ||
					pPlayer->GetTeam() == TEAM_SPECTATORS || m_VoteCloseTime)
					return;

				pPlayer->m_LastVoteTry = Now;
			}

			m_VoteType = VOTE_UNKNOWN;
			char aDesc[VOTE_DESC_LENGTH] = {0};
			char aCmd[VOTE_CMD_LENGTH] = {0};
			const char *pReason = pMsg->m_Reason[0] ? pMsg->m_Reason : "No reason given";

			if(str_comp_nocase(pMsg->m_Type, "option") == 0)
			{
				CVoteOptionServer *pOption = m_pVoteOptionFirst;
				while(pOption)
				{
					if(str_comp_nocase(pMsg->m_Value, pOption->m_aDescription) == 0)
					{
						str_format(aDesc, sizeof(aDesc), "%s", pOption->m_aDescription);
						str_format(aCmd, sizeof(aCmd), "%s", pOption->m_aCommand);
						char aBuf[128];
						str_format(aBuf, sizeof(aBuf),
							"'%d:%s' voted %s '%s' reason='%s' cmd='%s' force=%d",
							ClientID, Server()->ClientName(ClientID), pMsg->m_Type,
							aDesc, pReason, aCmd, pMsg->m_Force
						);
						Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
						if(pMsg->m_Force)
						{
							Server()->SetRconCID(ClientID);
							Console()->ExecuteLine(aCmd);
							Server()->SetRconCID(IServer::RCON_CID_SERV);
							ForceVote(VOTE_START_OP, aDesc, pReason);
							return;
						}
						m_VoteType = VOTE_START_OP;
						break;
					}

					pOption = pOption->m_pNext;
				}

				if(!pOption)
					return;
			}
			else if(str_comp_nocase(pMsg->m_Type, "kick") == 0)
			{
				if(!g_Config.m_SvVoteKick || m_pController->GetRealPlayerNum() < g_Config.m_SvVoteKickMin)
					return;

				int KickID = str_toint(pMsg->m_Value);
				if(KickID < 0 || KickID >= MAX_CLIENTS || !m_apPlayers[KickID] || KickID == ClientID || Server()->IsAuthed(KickID))
					return;

				str_format(aDesc, sizeof(aDesc), "%2d: %s", KickID, Server()->ClientName(KickID));
				if (!g_Config.m_SvVoteKickBantime)
					str_format(aCmd, sizeof(aCmd), "kick %d Kicked by vote", KickID);
				else
				{
					char aAddrStr[NETADDR_MAXSTRSIZE] = {0};
					Server()->GetClientAddr(KickID, aAddrStr, sizeof(aAddrStr));
					str_format(aCmd, sizeof(aCmd), "ban %s %d Banned by vote", aAddrStr, g_Config.m_SvVoteKickBantime);
				}
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf),
					"'%d:%s' voted %s '%d:%s' reason='%s' cmd='%s' force=%d",
					ClientID, Server()->ClientName(ClientID), pMsg->m_Type,
					KickID, Server()->ClientName(KickID), pReason, aCmd, pMsg->m_Force
				);
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
				if(pMsg->m_Force)
				{
					Server()->SetRconCID(ClientID);
					Console()->ExecuteLine(aCmd);
					Server()->SetRconCID(IServer::RCON_CID_SERV);
					return;
				}
				m_VoteType = VOTE_START_KICK;
				m_VoteClientID = KickID;
			}
			else if(str_comp_nocase(pMsg->m_Type, "spectate") == 0)
			{
				if(!g_Config.m_SvVoteSpectate)
					return;

				int SpectateID = str_toint(pMsg->m_Value);
				if(SpectateID < 0 || SpectateID >= MAX_CLIENTS || !m_apPlayers[SpectateID] || m_apPlayers[SpectateID]->GetTeam() == TEAM_SPECTATORS || SpectateID == ClientID)
					return;

				str_format(aDesc, sizeof(aDesc), "%2d: %s", SpectateID, Server()->ClientName(SpectateID));
				str_format(aCmd, sizeof(aCmd), "set_team %d -1 %d", SpectateID, g_Config.m_SvVoteSpectateRejoindelay);
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf),
					"'%d:%s' voted %s '%d:%s' reason='%s' cmd='%s' force=%d",
					ClientID, Server()->ClientName(ClientID), pMsg->m_Type,
					SpectateID, Server()->ClientName(SpectateID), pReason, aCmd, pMsg->m_Force
				);
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
				if(pMsg->m_Force)
				{
					Server()->SetRconCID(ClientID);
					Console()->ExecuteLine(aCmd);
					Server()->SetRconCID(IServer::RCON_CID_SERV);
					ForceVote(VOTE_START_SPEC, aDesc, pReason);
					return;
				}
				m_VoteType = VOTE_START_SPEC;
				m_VoteClientID = SpectateID;
			}

			if(m_VoteType != VOTE_UNKNOWN)
			{
				m_VoteCreator = ClientID;
				StartVote(aDesc, aCmd, pReason);
				pPlayer->m_Vote = 1;
				pPlayer->m_VotePos = m_VotePos = 1;
				pPlayer->m_LastVoteCall = Now;
			}
		}
		else if(MsgID == NETMSGTYPE_CL_VOTE)
		{
			if(!m_VoteCloseTime)
				return;

			if(pPlayer->m_Vote == 0)
			{
				CNetMsg_Cl_Vote *pMsg = (CNetMsg_Cl_Vote *)pRawMsg;
				if(!pMsg->m_Vote)
					return;

				pPlayer->m_Vote = pMsg->m_Vote;
				pPlayer->m_VotePos = ++m_VotePos;
				m_VoteUpdate = true;
			}
			else if(m_VoteCreator == pPlayer->GetCID())
			{
				CNetMsg_Cl_Vote *pMsg = (CNetMsg_Cl_Vote *)pRawMsg;
				if(pMsg->m_Vote != -1 || m_VoteCancelTime<time_get())
					return;

				m_VoteCloseTime = -1;
			}
		}
		else if(MsgID == NETMSGTYPE_CL_SETTEAM && m_pController->IsTeamChangeAllowed())
		{
			CNetMsg_Cl_SetTeam *pMsg = (CNetMsg_Cl_SetTeam *)pRawMsg;

			if(pPlayer->GetTeam() == pMsg->m_Team ||
				(g_Config.m_SvSpamprotection && pPlayer->m_LastSetTeam && pPlayer->m_LastSetTeam+Server()->TickSpeed()*3 > Server()->Tick()) ||
				(pMsg->m_Team != TEAM_SPECTATORS && m_LockTeams) || pPlayer->m_TeamChangeTick > Server()->Tick())
				return;

			if(pPlayer->GetCharacter() && pPlayer->GetCharacter()->m_FreezeTime)
			{
				SendChatTarget(ClientID, "You are not allowed to change team while fronzen.");
				return;
			}

			pPlayer->m_LastSetTeam = Server()->Tick();

			// Switch team on given client and kill/respawn him
			if(m_pController->CanJoinTeam(pMsg->m_Team, ClientID) && m_pController->CanChangeTeam(pPlayer, pMsg->m_Team))
			{
				if(pPlayer->GetTeam() == TEAM_SPECTATORS || pMsg->m_Team == TEAM_SPECTATORS)
					m_VoteUpdate = true;
				pPlayer->m_TeamChangeTick = Server()->Tick()+Server()->TickSpeed()*3;
				m_pController->DoTeamChange(pPlayer, pMsg->m_Team);
			}
		}
		else if (MsgID == NETMSGTYPE_CL_SETSPECTATORMODE && !m_World.m_Paused)
		{
			CNetMsg_Cl_SetSpectatorMode *pMsg = (CNetMsg_Cl_SetSpectatorMode *)pRawMsg;

			if(g_Config.m_SvSpamprotection && pPlayer->m_LastSetSpectatorMode && pPlayer->m_LastSetSpectatorMode+Server()->TickSpeed() > Server()->Tick())
				return;

			pPlayer->m_LastSetSpectatorMode = Server()->Tick();
			if(!pPlayer->SetSpectatorID(pMsg->m_SpecMode, pMsg->m_SpectatorID))
				SendGameMsg(GAMEMSG_SPEC_INVALIDID, ClientID);
		}
		else if (MsgID == NETMSGTYPE_CL_EMOTICON && !m_World.m_Paused)
		{
			CNetMsg_Cl_Emoticon *pMsg = (CNetMsg_Cl_Emoticon *)pRawMsg;

			if(g_Config.m_SvSpamprotection && pPlayer->m_LastEmote && pPlayer->m_LastEmote+Server()->TickSpeed()*g_Config.m_SvEmoticonDelay > Server()->Tick())
				return;

			pPlayer->m_LastEmote = Server()->Tick();

			SendEmoticon(ClientID, pMsg->m_Emoticon);
		}
		else if (MsgID == NETMSGTYPE_CL_KILL && !m_World.m_Paused)
		{
			if(pPlayer->m_LastKill && pPlayer->m_LastKill+Server()->TickSpeed()*3 > Server()->Tick())
				return;
			if(pPlayer->GetCharacter() && pPlayer->GetCharacter()->m_FreezeTime)
				return;
			pPlayer->m_LastKill = Server()->Tick();
			pPlayer->KillCharacter(WEAPON_SELF);
		}
		else if (MsgID == NETMSGTYPE_CL_READYCHANGE)
		{
			if(pPlayer->m_LastReadyChange && pPlayer->m_LastReadyChange+Server()->TickSpeed()*1 > Server()->Tick())
				return;

			pPlayer->m_LastReadyChange = Server()->Tick();
			m_pController->OnPlayerReadyChange(pPlayer);
		}
		else if(MsgID == NETMSGTYPE_CL_SKINCHANGE)
		{
			if(pPlayer->m_LastChangeInfo && pPlayer->m_LastChangeInfo+Server()->TickSpeed()*5 > Server()->Tick())
				return;

			pPlayer->m_LastChangeInfo = Server()->Tick();
			CNetMsg_Cl_SkinChange *pMsg = (CNetMsg_Cl_SkinChange *)pRawMsg;

			for(int p = 0; p < NUM_SKINPARTS; p++)
			{
				str_copy(pPlayer->m_TeeInfos.m_aaSkinPartNames[p], pMsg->m_apSkinPartNames[p], 24);
				pPlayer->m_TeeInfos.m_aUseCustomColors[p] = pMsg->m_aUseCustomColors[p];
				pPlayer->m_TeeInfos.m_aSkinPartColors[p] = pMsg->m_aSkinPartColors[p];
			}

			// update all clients
			for(int i = 0; i < MAX_CLIENTS; ++i)
			{
				if(!m_apPlayers[i] || (!Server()->ClientIngame(i) && !m_apPlayers[i]->IsDummy()) || Server()->GetClientVersion(i) < MIN_SKINCHANGE_CLIENTVERSION)
					continue;

				SendSkinChange(pPlayer->GetCID(), i);
			}

			m_pController->OnPlayerInfoChange(pPlayer);
		}
		else if (MsgID == NETMSGTYPE_CL_COMMAND)
		{
			CNetMsg_Cl_Command *pMsg = (CNetMsg_Cl_Command*)pRawMsg;
			m_pController->OnPlayerCommand(pPlayer, pMsg->m_Name, pMsg->m_Arguments);
		}
	}
	else
	{
		if (MsgID == NETMSGTYPE_CL_STARTINFO)
		{
			if(pPlayer->m_IsReadyToEnter)
				return;

			CNetMsg_Cl_StartInfo *pMsg = (CNetMsg_Cl_StartInfo *)pRawMsg;
			pPlayer->m_LastChangeInfo = Server()->Tick();

			// set start infos
			Server()->SetClientName(ClientID, pMsg->m_pName);
			Server()->SetClientClan(ClientID, pMsg->m_pClan);
			Server()->SetClientCountry(ClientID, pMsg->m_Country);

			for(int p = 0; p < NUM_SKINPARTS; p++)
			{
				str_copy(pPlayer->m_TeeInfos.m_aaSkinPartNames[p], pMsg->m_apSkinPartNames[p], 24);
				pPlayer->m_TeeInfos.m_aUseCustomColors[p] = pMsg->m_aUseCustomColors[p];
				pPlayer->m_TeeInfos.m_aSkinPartColors[p] = pMsg->m_aSkinPartColors[p];
			}

			m_pController->OnPlayerInfoChange(pPlayer);

			// send vote options
			CNetMsg_Sv_VoteClearOptions ClearMsg;
			Server()->SendPackMsg(&ClearMsg, MSGFLAG_VITAL, ClientID);

			CVoteOptionServer *pCurrent = m_pVoteOptionFirst;
			while(pCurrent)
			{
				// count options for actual packet
				int NumOptions = 0;
				for(CVoteOptionServer *p = pCurrent; p && NumOptions < MAX_VOTE_OPTION_ADD; p = p->m_pNext, ++NumOptions);

				// pack and send vote list packet
				CMsgPacker Msg(NETMSGTYPE_SV_VOTEOPTIONLISTADD);
				Msg.AddInt(NumOptions);
				while(pCurrent && NumOptions--)
				{
					Msg.AddString(pCurrent->m_aDescription, VOTE_DESC_LENGTH);
					pCurrent = pCurrent->m_pNext;
				}
				Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
			}

			// send tuning parameters to client
			SendTuningParams(ClientID);

			// client is ready to enter
			pPlayer->m_IsReadyToEnter = true;
			CNetMsg_Sv_ReadyToEnter m;
			Server()->SendPackMsg(&m, MSGFLAG_VITAL|MSGFLAG_FLUSH, ClientID);
		}
	}
}

void CGameContext::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pParamName = pResult->GetString(0);
	float NewValue = pResult->GetFloat(1);

	if(pSelf->Tuning()->Set(pParamName, NewValue))
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
		pSelf->SendTuningParams(-1);
	}
	else
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
}

void CGameContext::ConTuneReset(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CTuningParams TuningParams;
	*pSelf->Tuning() = TuningParams;
	pSelf->SendTuningParams(-1);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "Tuning reset");
}

void CGameContext::ConTuneDump(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	char aBuf[256];
	for(int i = 0; i < pSelf->Tuning()->Num(); i++)
	{
		float v;
		pSelf->Tuning()->Get(i, &v);
		str_format(aBuf, sizeof(aBuf), "%s %.2f", pSelf->Tuning()->m_apNames[i], v);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	}
}

void CGameContext::ConPause(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	if(pResult->NumArguments())
		pSelf->m_pController->DoPause(clamp(pResult->GetInteger(0), -1, 1000));
	else
		pSelf->m_pController->DoPause(pSelf->m_pController->IsGamePaused() ? 0 : IGameController::TIMER_INFINITE);
}

void CGameContext::ConChangeMap(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_pController->ChangeMap(pResult->NumArguments() ? pResult->GetString(0) : "");
}

void CGameContext::ConRestart(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(pResult->NumArguments())
		pSelf->m_pController->DoWarmup(clamp(pResult->GetInteger(0), -1, 1000));
	else
		pSelf->m_pController->DoWarmup(0);
}

void CGameContext::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->SendChat(-1, CHAT_ALL, -1, pResult->GetString(0));
}

void CGameContext::ConBroadcast(IConsole::IResult* pResult, void* pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->SendBroadcast(pResult->GetString(0), -1);
}

void CGameContext::ConSetTeam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int ClientID = clamp(pResult->GetInteger(0), 0, (int)MAX_CLIENTS-1);
	int Team = clamp(pResult->GetInteger(1), -1, 1);
	int Delay = pResult->NumArguments()>2 ? pResult->GetInteger(2) : 0;
	if(!pSelf->m_apPlayers[ClientID] || !pSelf->m_pController->CanJoinTeam(Team, ClientID))
		return;

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "moved client %d to team %d", ClientID, Team);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	pSelf->m_apPlayers[ClientID]->m_TeamChangeTick = pSelf->Server()->Tick()+pSelf->Server()->TickSpeed()*Delay*60;
	pSelf->m_pController->DoTeamChange(pSelf->m_apPlayers[ClientID], Team);
}

void CGameContext::ConSetTeamAll(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int Team = clamp(pResult->GetInteger(0), -1, 1);

	pSelf->SendGameMsg(GAMEMSG_TEAM_ALL, Team, -1);

	for(int i = 0; i < MAX_CLIENTS; ++i)
		if(pSelf->m_apPlayers[i] && pSelf->m_pController->CanJoinTeam(Team, i))
			pSelf->m_pController->DoTeamChange(pSelf->m_apPlayers[i], Team, false);
}

void CGameContext::ConSwapTeams(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->SwapTeams();
}

void CGameContext::ConShuffleTeams(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!pSelf->m_pController->IsTeamplay())
		return;

	int rnd = 0;
	int PlayerTeam = 0;
	int aPlayer[MAX_CLIENTS];

	for(int i = 0; i < MAX_CLIENTS; i++)
		if(pSelf->m_apPlayers[i] && pSelf->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
			aPlayer[PlayerTeam++]=i;

	pSelf->SendGameMsg(GAMEMSG_TEAM_SHUFFLE, -1);

	//creating random permutation
	for(int i = PlayerTeam; i > 1; i--)
	{
		rnd = random_int() % i;
		int tmp = aPlayer[rnd];
		aPlayer[rnd] = aPlayer[i-1];
		aPlayer[i-1] = tmp;
	}
	//uneven Number of Players?
	rnd = PlayerTeam % 2 ? random_int() % 2 : 0;

	for(int i = 0; i < PlayerTeam; i++)
		pSelf->m_pController->DoTeamChange(pSelf->m_apPlayers[aPlayer[i]], i < (PlayerTeam+rnd)/2 ? TEAM_RED : TEAM_BLUE, false);
}

void CGameContext::ConLockTeams(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_LockTeams ^= 1;
	pSelf->SendSettings(-1);
}

void CGameContext::ConForceTeamBalance(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_pController->ForceTeamBalance();
}

void CGameContext::ConAddVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pDescription = pResult->GetString(0);
	const char *pCommand = pResult->GetString(1);

	if(pSelf->m_NumVoteOptions == MAX_VOTE_OPTIONS)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "maximum number of vote options reached");
		return;
	}

	// check for valid option
	if(!pSelf->Console()->LineIsValid(pCommand) || str_length(pCommand) >= VOTE_CMD_LENGTH)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid command '%s'", pCommand);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}
	while(*pDescription == ' ')
		pDescription++;
	if(str_length(pDescription) >= VOTE_DESC_LENGTH || *pDescription == 0)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid option '%s'", pDescription);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// check for duplicate entry
	CVoteOptionServer *pOption = pSelf->m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription) == 0)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "option '%s' already exists", pDescription);
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			return;
		}
		pOption = pOption->m_pNext;
	}

	// add the option
	++pSelf->m_NumVoteOptions;
	int Len = str_length(pCommand);

	pOption = (CVoteOptionServer *)pSelf->m_pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len);
	pOption->m_pNext = 0;
	pOption->m_pPrev = pSelf->m_pVoteOptionLast;
	if(pOption->m_pPrev)
		pOption->m_pPrev->m_pNext = pOption;
	pSelf->m_pVoteOptionLast = pOption;
	if(!pSelf->m_pVoteOptionFirst)
		pSelf->m_pVoteOptionFirst = pOption;

	str_copy(pOption->m_aDescription, pDescription, sizeof(pOption->m_aDescription));
	mem_copy(pOption->m_aCommand, pCommand, Len+1);
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "added option '%s' '%s'", pOption->m_aDescription, pOption->m_aCommand);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	// inform clients about added option
	CNetMsg_Sv_VoteOptionAdd OptionMsg;
	OptionMsg.m_pDescription = pOption->m_aDescription;
	pSelf->Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, -1);
}

void CGameContext::ConRemoveVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pDescription = pResult->GetString(0);

	// check for valid option
	CVoteOptionServer *pOption = pSelf->m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription) == 0)
			break;
		pOption = pOption->m_pNext;
	}
	if(!pOption)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "option '%s' does not exist", pDescription);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// inform clients about removed option
	CNetMsg_Sv_VoteOptionRemove OptionMsg;
	OptionMsg.m_pDescription = pOption->m_aDescription;
	pSelf->Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, -1);

	// TODO: improve this
	// remove the option
	--pSelf->m_NumVoteOptions;
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "removed option '%s' '%s'", pOption->m_aDescription, pOption->m_aCommand);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	CHeap *pVoteOptionHeap = new CHeap();
	CVoteOptionServer *pVoteOptionFirst = 0;
	CVoteOptionServer *pVoteOptionLast = 0;
	int NumVoteOptions = pSelf->m_NumVoteOptions;
	for(CVoteOptionServer *pSrc = pSelf->m_pVoteOptionFirst; pSrc; pSrc = pSrc->m_pNext)
	{
		if(pSrc == pOption)
			continue;

		// copy option
		int Len = str_length(pSrc->m_aCommand);
		CVoteOptionServer *pDst = (CVoteOptionServer *)pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len);
		pDst->m_pNext = 0;
		pDst->m_pPrev = pVoteOptionLast;
		if(pDst->m_pPrev)
			pDst->m_pPrev->m_pNext = pDst;
		pVoteOptionLast = pDst;
		if(!pVoteOptionFirst)
			pVoteOptionFirst = pDst;

		str_copy(pDst->m_aDescription, pSrc->m_aDescription, sizeof(pDst->m_aDescription));
		mem_copy(pDst->m_aCommand, pSrc->m_aCommand, Len+1);
	}

	// clean up
	delete pSelf->m_pVoteOptionHeap;
	pSelf->m_pVoteOptionHeap = pVoteOptionHeap;
	pSelf->m_pVoteOptionFirst = pVoteOptionFirst;
	pSelf->m_pVoteOptionLast = pVoteOptionLast;
	pSelf->m_NumVoteOptions = NumVoteOptions;
}

void CGameContext::ConClearVotes(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "cleared votes");
	CNetMsg_Sv_VoteClearOptions VoteClearOptionsMsg;
	pSelf->Server()->SendPackMsg(&VoteClearOptionsMsg, MSGFLAG_VITAL, -1);
	pSelf->m_pVoteOptionHeap->Reset();
	pSelf->m_pVoteOptionFirst = 0;
	pSelf->m_pVoteOptionLast = 0;
	pSelf->m_NumVoteOptions = 0;
}

void CGameContext::ConVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	// check if there is a vote running
	if(!pSelf->m_VoteCloseTime)
		return;

	if(str_comp_nocase(pResult->GetString(0), "yes") == 0)
		pSelf->m_VoteEnforce = CGameContext::VOTE_ENFORCE_YES;
	else if(str_comp_nocase(pResult->GetString(0), "no") == 0)
		pSelf->m_VoteEnforce = CGameContext::VOTE_ENFORCE_NO;
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "forcing vote %s", pResult->GetString(0));
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

void CGameContext::ConchainSpecialMotdupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameContext *pSelf = (CGameContext *)pUserData;
		pSelf->SendMotd(-1);
	}
}

void CGameContext::ConchainSettingUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameContext *pSelf = (CGameContext *)pUserData;
		if(pSelf->Server()->MaxClients() < g_Config.m_SvPlayerSlots)
			g_Config.m_SvPlayerSlots = pSelf->Server()->MaxClients();
		pSelf->SendSettings(-1);
	}
}

void CGameContext::ConchainGameinfoUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameContext *pSelf = (CGameContext *)pUserData;
		if(pSelf->m_pController)
			pSelf->m_pController->CheckGameInfo();
	}
}

void CGameContext::OnConsoleInit()
{
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();

	Console()->Register("tune", "si", CFGFLAG_SERVER, ConTuneParam, this, "Tune variable to value");
	Console()->Register("tune_reset", "", CFGFLAG_SERVER, ConTuneReset, this, "Reset tuning");
	Console()->Register("tune_dump", "", CFGFLAG_SERVER, ConTuneDump, this, "Dump tuning");

	Console()->Register("pause", "?i", CFGFLAG_SERVER|CFGFLAG_STORE, ConPause, this, "Pause/unpause game");
	Console()->Register("change_map", "?r", CFGFLAG_SERVER|CFGFLAG_STORE, ConChangeMap, this, "Change map");
	Console()->Register("restart", "?i", CFGFLAG_SERVER|CFGFLAG_STORE, ConRestart, this, "Restart in x seconds (0 = abort)");
	Console()->Register("say", "r", CFGFLAG_SERVER, ConSay, this, "Say in chat");
	Console()->Register("broadcast", "r", CFGFLAG_SERVER, ConBroadcast, this, "Broadcast message");
	Console()->Register("set_team", "ii?i", CFGFLAG_SERVER, ConSetTeam, this, "Set team of player to team");
	Console()->Register("set_team_all", "i", CFGFLAG_SERVER, ConSetTeamAll, this, "Set team of all players to team");
	Console()->Register("swap_teams", "", CFGFLAG_SERVER, ConSwapTeams, this, "Swap the current teams");
	Console()->Register("shuffle_teams", "", CFGFLAG_SERVER, ConShuffleTeams, this, "Shuffle the current teams");
	Console()->Register("lock_teams", "", CFGFLAG_SERVER, ConLockTeams, this, "Lock/unlock teams");
	Console()->Register("force_teambalance", "", CFGFLAG_SERVER, ConForceTeamBalance, this, "Force team balance");

	Console()->Register("add_vote", "sr", CFGFLAG_SERVER, ConAddVote, this, "Add a voting option");
	Console()->Register("remove_vote", "s", CFGFLAG_SERVER, ConRemoveVote, this, "remove a voting option");
	Console()->Register("clear_votes", "", CFGFLAG_SERVER, ConClearVotes, this, "Clears the voting options");
	Console()->Register("vote", "r", CFGFLAG_SERVER, ConVote, this, "Force a vote to yes/no");
}

void CGameContext::OnInit()
{
	// init everything
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_World.SetGameServer(this);
	m_Events.SetGameServer(this);

	// HACK: only set static size for items, which were available in the first 0.7 release
	// so new items don't break the snapshot delta
	static const int OLD_NUM_NETOBJTYPES = 23;
	for(int i = 0; i < OLD_NUM_NETOBJTYPES; i++)
		Server()->SnapSetStaticsize(i, m_NetObjHandler.GetObjSize(i));

	m_Layers.Init(Kernel());
	m_Collision.Init(&m_Layers);

	// select gametype
	if(str_comp_nocase(g_Config.m_SvGametype, "solofng") == 0)
		m_pController = new CGameControllerMOD(this);
	else if(str_comp_nocase(g_Config.m_SvGametype, "ctf") == 0)
		m_pController = new CGameControllerCTF(this);
	else if(str_comp_nocase(g_Config.m_SvGametype, "lms") == 0)
		m_pController = new CGameControllerLMS(this);
	else if(str_comp_nocase(g_Config.m_SvGametype, "lts") == 0)
		m_pController = new CGameControllerLTS(this);
	else if(str_comp_nocase(g_Config.m_SvGametype, "tdm") == 0)
		m_pController = new CGameControllerTDM(this);
	else
		m_pController = new CGameControllerDM(this);

	// create all entities from the game layer
	CMapItemLayerTilemap *pTileMap = m_Layers.GameLayer();
	CTile *pTiles = (CTile *)Kernel()->RequestInterface<IMap>()->GetData(pTileMap->m_Data);
	for(int y = 0; y < pTileMap->m_Height; y++)
	{
		for(int x = 0; x < pTileMap->m_Width; x++)
		{
			int Index = pTiles[y*pTileMap->m_Width+x].m_Index;

			if(Index >= ENTITY_OFFSET)
			{
				vec2 Pos(x*32.0f+16.0f, y*32.0f+16.0f);
				m_pController->OnEntity(Index-ENTITY_OFFSET, Pos);
			}
		}
	}

	Console()->Chain("sv_motd", ConchainSpecialMotdupdate, this);

	Console()->Chain("sv_vote_kick", ConchainSettingUpdate, this);
	Console()->Chain("sv_vote_kick_min", ConchainSettingUpdate, this);
	Console()->Chain("sv_vote_spectate", ConchainSettingUpdate, this);
	Console()->Chain("sv_teambalance_time", ConchainSettingUpdate, this);
	Console()->Chain("sv_player_slots", ConchainSettingUpdate, this);

	Console()->Chain("sv_scorelimit", ConchainGameinfoUpdate, this);
	Console()->Chain("sv_timelimit", ConchainGameinfoUpdate, this);
	Console()->Chain("sv_matches_per_map", ConchainGameinfoUpdate, this);

	// clamp sv_player_slots to 0..MaxClients
	if(Server()->MaxClients() < g_Config.m_SvPlayerSlots)
		g_Config.m_SvPlayerSlots = Server()->MaxClients();

#ifdef CONF_DEBUG
	// clamp dbg_dummies to 0..MaxClients-1
	if(Server()->MaxClients() <= g_Config.m_DbgDummies)
		g_Config.m_DbgDummies = Server()->MaxClients();
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies ; i++)
			OnClientConnected(Server()->MaxClients() -i-1, true, false);
	}
#endif

	// solofng

	if (TestSaveStats())
		exit(1);
}

void CGameContext::OnShutdown()
{
	delete m_pController;
	m_pController = 0;
	Clear();
}

void CGameContext::OnSnap(int ClientID)
{
	// add tuning to demo
	CTuningParams StandardTuning;
	if(ClientID == -1 && Server()->DemoRecorder_IsRecording() && mem_comp(&StandardTuning, &m_Tuning, sizeof(CTuningParams)) != 0)
	{
		CNetObj_De_TuneParams *pTuneParams = static_cast<CNetObj_De_TuneParams *>(Server()->SnapNewItem(NETOBJTYPE_DE_TUNEPARAMS, 0, sizeof(CNetObj_De_TuneParams)));
		if(!pTuneParams)
			return;

		mem_copy(pTuneParams->m_aTuneParams, &m_Tuning, sizeof(pTuneParams->m_aTuneParams));
	}

	m_World.Snap(ClientID);
	m_pController->Snap(ClientID);
	m_Events.Snap(ClientID);

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
			m_apPlayers[i]->Snap(ClientID);
	}
}
void CGameContext::OnPreSnap() {}
void CGameContext::OnPostSnap()
{
	m_World.PostSnap();
	m_Events.Clear();
}

bool CGameContext::IsClientReady(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->m_IsReadyToEnter;
}

bool CGameContext::IsClientPlayer(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetTeam() != TEAM_SPECTATORS;
}

bool CGameContext::IsClientSpectator(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS;
}

void CGameContext::ShowStatsMeta(int ClientID, const char *pName)
{
	char aBuf[128];
	char aName[64];
	str_copy(aName, pName, sizeof(aName));
	str_clean_whitespaces_simple(aName);
	CFngStats Stats;
	if (LoadStats(ClientID, aName, &Stats) != 0)
	{
		str_format(aBuf, sizeof(aBuf), "[stats] player '%s' not found.", aName);
		SendChatTarget(ClientID, aBuf);
		return;
	}
	PrintStatsMeta(ClientID, &Stats);
}

void CGameContext::PrintStatsMeta(int ClientID, const CFngStats *pStats)
{
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "=== '%s' ===", pStats->m_aName);
	SendChatTarget(ClientID, aBuf);
	str_format(aBuf, sizeof(aBuf), "Clan: %s", pStats->m_aClan);
	SendChatTarget(ClientID, aBuf);
	str_format(aBuf, sizeof(aBuf), "Config: %d", pStats->m_CfgFlags);
	SendChatTarget(ClientID, aBuf);
	str_format(aBuf, sizeof(aBuf), "- hammertune: %s", pStats->m_CfgFlags&CFG_VANILLA_HAMMER ? "vanilla" : "fng");
	SendChatTarget(ClientID, aBuf);
	int minutes = (pStats->m_TotalOnlineTime % 3600) / 60;
	int hours = (pStats->m_TotalOnlineTime % 86400) / 3600;
	int days = (pStats->m_TotalOnlineTime % (86400 * 30)) / 86400;
	str_format(aBuf, sizeof(aBuf), "Online time: %02d:%02d:%02d (%lds)", days, hours, minutes, pStats->m_TotalOnlineTime);
	SendChatTarget(ClientID, aBuf);
	strftime(aBuf, sizeof(aBuf), "%F %r", localtime(&(pStats->m_FirstSeen)));
	SendChatTarget(ClientID, "First seen:");
	SendChatTarget(ClientID, aBuf);
	strftime(aBuf, sizeof(aBuf), "%F %r", localtime(&(pStats->m_LastSeen)));
	SendChatTarget(ClientID, "Last seen:");
	SendChatTarget(ClientID, aBuf);
}

void CGameContext::PrintStats(int ClientID, const CFngStats *pStats)
{
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "=== '%s' ===", pStats->m_aName);
	SendChatTarget(ClientID, aBuf);
	str_format(aBuf, sizeof(aBuf), "K/D %d/%d", pStats->m_Kills, pStats->m_Deaths);
	SendChatTarget(ClientID, aBuf);
	str_format(aBuf, sizeof(aBuf), "Freezes/Frozen %d/%d", pStats->m_Freezes, pStats->m_Frozen);
	SendChatTarget(ClientID, aBuf);
	str_format(aBuf, sizeof(aBuf), "Shots %d", pStats->m_RifleShots);
	SendChatTarget(ClientID, aBuf);
	str_format(aBuf, sizeof(aBuf), "Spree %d", pStats->m_SpreeBest);
	SendChatTarget(ClientID, aBuf);
	str_format(aBuf, sizeof(aBuf), "Multi %d", pStats->m_MultiBest);
	SendChatTarget(ClientID, aBuf);
	str_format(aBuf, sizeof(aBuf), "Spikes Gold: %d Green: %d Purple: %d", pStats->m_GoldSpikes, pStats->m_GreenSpikes, pStats->m_PurpleSpikes);
	SendChatTarget(ClientID, aBuf);
	for (int i = 0; i < MAX_MULTIS; i++)
	{
		if (!pStats->m_aMultis[i])
			continue;
		str_format(aBuf, sizeof(aBuf), "x%d multis %d", i+2, pStats->m_aMultis[i]);
		SendChatTarget(ClientID, aBuf);
	}
}

void CGameContext::ShowRoundStats(int ClientID, const char *pName)
{
	char aBuf[512];
	int StatsID = GetCIDByName(pName);
	if (StatsID == -1)
	{
		str_format(aBuf, sizeof(aBuf), "[stats] '%s' is not online.", pName);
		SendChatTarget(ClientID, aBuf);
		return;
	}
	CPlayer *pPlayer = m_apPlayers[StatsID];
	if (!pPlayer)
	{
		SendChatTarget(ClientID, "[stats] unexpected error please contact an admin.");
		return;
	}
	pPlayer->InitRoundStats();
	PrintStats(ClientID, pPlayer->GetRoundStats());
}

void CGameContext::ShowStats(int ClientID, const char *pName)
{
	char aBuf[128];
	char aName[64];
	str_copy(aName, pName, sizeof(aName));
	str_clean_whitespaces_simple(aName);
	CFngStats Stats;
	if (LoadStats(ClientID, aName, &Stats) != 0)
	{
		str_format(aBuf, sizeof(aBuf), "[stats] player '%s' not found.", aName);
		SendChatTarget(ClientID, aBuf);
		return;
	}
	PrintStats(ClientID, &Stats);
}

/*
// TODO: implemt /rank_kills etc
static bool cmpKills(const CFngStats *a, const CFngStats *b)
{
    return a->m_Kills > b->m_Kills;
}
*/

static bool cmpTmp(const CFngStats *a, const CFngStats *b)
{
    return a->m_Tmp > b->m_Tmp;
}

void CGameContext::TopThread(void *pArg)
{
	DIR *pDir;
	struct dirent *pDe;
	int load;
	int start;
	int err = 0;
	int row_index = 0;
	char aFilePath[MAX_FILE_PATH];
	std::vector<CFngStats*> m_vpStats;
	CGameContext *pGS = static_cast<CGameContext*>(pArg);
	int top = pGS->m_RankThreadTop - 1;
	int r = top;
	if (top == -1) // top5 0 is same as top5 1
		top = r = 0;
	dbg_msg("top_thread", "init top thread gs=%p rankthreadtop=%d top=%d", pGS, pGS->m_RankThreadTop, top);
	char aFilename[MAX_FILE_LEN];
	if (escape_filename(aFilename, sizeof(aFilename), pGS->m_aRankThreadName))
	{
		str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] save failed: escape error.");
		err = 1; goto end;
	}
	pDir = opendir(g_Config.m_SvStatsPath);
	if(pDir == NULL)
	{
		str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] failed to open directory '%s'.", g_Config.m_SvStatsPath);
		err = 1; goto end;
	}
	while ((pDe = readdir(pDir)) != NULL)
	{
		if (str_endswith(pDe->d_name, ".lck"))
		{
			dbg_msg("top_thread", "file '%s' is locked by write.", pDe->d_name);
			str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] top command failed try again later.");
			err = 1; goto end;
		}
		if (!str_comp(pDe->d_name, ".") || !str_comp(pDe->d_name, ".."))
		{
			// printf("ignore '%s'.\n", pDe->d_name);
			continue;
		}
		if (!str_endswith(pDe->d_name, ".acc"))
		{
			// printf("warning not a .acc file '%s'.\n", pDe->d_name);
			continue;
		}
		str_format(aFilePath, sizeof(aFilePath), "%s/%s", g_Config.m_SvStatsPath, pDe->d_name);
		// printf("load stats '%s' '%s' ... \n", pDe->d_name, aFilePath);
		CFngStats *pStats = (struct CFngStats*)malloc(sizeof(struct CFngStats));
		if (!pStats)
		{
			str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] top command failed (malloc).");
			err = 1; goto end;
		}
		load = pGS->LoadStatsFile(-1, aFilePath, pStats);
		if (load)
		{
			dbg_msg("top_thread", "file '%s' failed to load with err=%d", aFilePath, load);
			str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] top command failed try again later.");
			free(pStats);
			err = 1; goto end;
		}
		pStats->m_Tmp = pGS->CalcScore(pStats);
		m_vpStats.push_back(pStats);
		// dbg_msg("top_thread", "pushing back '%s' kills: %d", pStats->m_aName, pStats->m_Kills);
	}
	std::sort(m_vpStats.begin(), m_vpStats.end(), cmpTmp);
	// negative top = starting from worst
	if (top < 0)
	{
		top -= 3;
		start = m_vpStats.size() + top;
		r = start;
		// dbg_msg("top_thread", "%d - %lu", start, m_vpStats.size());
		if (start < 0)
		{
			dbg_msg("top_thread", "start=%d is negative aborting...", start);
			str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] argument too low");
			err = 1; goto end;
		}
		for(std::vector<CFngStats*>::size_type i = start; i < m_vpStats.size(); i++)
		{
			if (++r > start+5)
				break;
			str_format(
				pGS->m_aRankThreadResult[row_index], sizeof(pGS->m_aRankThreadResult[row_index]),
				"%lu. '%s' score %d",
				i+1, m_vpStats[i]->m_aName, pGS->CalcScore(m_vpStats[i])
			);
			row_index++;
		}
	}
	else
	{
		for(std::vector<CFngStats*>::size_type i = top; i < m_vpStats.size(); i++)
		{
			if (++r > top+5)
				break;
			str_format(
				pGS->m_aRankThreadResult[row_index], sizeof(pGS->m_aRankThreadResult[row_index]),
				"%d. '%s' score %d",
				r, m_vpStats[i]->m_aName, pGS->CalcScore(m_vpStats[i])
			);
			row_index++;
		}
	}
	end:
	closedir(pDir);
	for(std::vector<CFngStats*>::size_type i = 0; i != m_vpStats.size(); i++)
		free(m_vpStats[i]);
	pGS->m_RankThreadState = err ? RT_ERR : RT_DONE;
}

void CGameContext::RankThread(void *pArg)
{
	DIR *pDir;
	struct dirent *pDe;
	int Rank;
	int Score;
	int err = 0;
	int load;
	char aFilePath[MAX_FILE_PATH];
	std::vector<CFngStats*> m_vpStats;
	CGameContext *pGS = static_cast<CGameContext*>(pArg);
	dbg_msg("rank_thread", "init rank thread gs=%p name=%s", pGS, pGS->m_aRankThreadName);
	CFngStats Stats;
	if (pGS->LoadStats(-1, pGS->m_aRankThreadName, &Stats) != 0)
	{
		str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] player '%s' is not ranked yet.", pGS->m_aRankThreadName);
		err = 1; goto end;
	}

	char aFilename[MAX_FILE_LEN];
	if (escape_filename(aFilename, sizeof(aFilename), pGS->m_aRankThreadName))
	{
		str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] save failed: escape error.");
		err = 1; goto end;
	}
	pDir = opendir(g_Config.m_SvStatsPath);
	if(pDir == NULL)
	{
		str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] failed to open directory '%s'.", g_Config.m_SvStatsPath);
		err = 1; goto end;
	}
	while ((pDe = readdir(pDir)) != NULL)
	{
		if (str_endswith(pDe->d_name, ".lck"))
		{
			dbg_msg("rank_thread", "file '%s' is locked by write.", pDe->d_name);
			str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] rank command failed try again later.");
			err = 1; goto end;
		}
		if (!str_comp(pDe->d_name, ".") || !str_comp(pDe->d_name, ".."))
		{
			// printf("ignore '%s'.\n", pDe->d_name);
			continue;
		}
		if (!str_endswith(pDe->d_name, ".acc"))
		{
			// printf("warning not a .acc file '%s'.\n", pDe->d_name);
			continue;
		}
		str_format(aFilePath, sizeof(aFilePath), "%s/%s", g_Config.m_SvStatsPath, pDe->d_name);
		// printf("load stats '%s' '%s' ... \n", pDe->d_name, aFilePath);
		CFngStats *pStats = (struct CFngStats*)malloc(sizeof(struct CFngStats));
		if (!pStats)
		{
			str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] rank command failed (malloc).");
			err = 1; goto end;
		}
		load = pGS->LoadStatsFile(-1, aFilePath, pStats);
		if (load)
		{
			dbg_msg("rank_thread", "file '%s' failed to load (err=%d)", aFilePath, load);
			str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "[stats] rank command failed try again later.");
			free(pStats);
			err = 1; goto end;
		}
		pStats->m_Tmp = pGS->CalcScore(pStats);
		m_vpStats.push_back(pStats);
		// dbg_msg("rank_thread", "pushing back '%s' kills: %d", pStats->m_aName, pStats->m_Kills);
	}
	std::sort(m_vpStats.begin(), m_vpStats.end(), cmpTmp);
	// TOP5
	/*
	for(std::vector<CFngStats*>::size_type i = 0; i != m_vpStats.size(); i++)
	{
		if (++r > 5)
			break;
		dbg_msg("rank", "%d. '%s' kills: %d", r, m_vpStats[i]->m_aName, m_vpStats[i]->m_Kills);
	}
	*/
	for(std::vector<CFngStats*>::size_type i = 0; i != m_vpStats.size(); i++)
	{
		if (!str_comp(pGS->m_aRankThreadName, m_vpStats[i]->m_aName))
		{
			Rank = i+1;
			break;
		}
	}

	Score = pGS->CalcScore(&Stats);
	str_format(pGS->m_aRankThreadResult[0], sizeof(pGS->m_aRankThreadResult[0]), "%d. '%s' score %d (requested by '%s')",
		Rank, pGS->m_aRankThreadName, Score, pGS->m_aRankThreadRequestName);

	end:
	closedir(pDir);
	for(std::vector<CFngStats*>::size_type i = 0; i != m_vpStats.size(); i++)
		free(m_vpStats[i]);
	pGS->m_RankThreadState = err ? RT_ERR : RT_DONE;
}

void CGameContext::ShowTopScore(int ClientID, int Top)
{
	if (m_RankThreadState != RT_IDLE)
	{
		SendChatTarget(ClientID, "[stats] rank is currently being requested try agian later.");
		return;
	}
	m_RankThreadState = RT_ACTIVE;
	m_RankThreadTop = Top;
	m_RankThreadReqID =  ClientID;
	m_RankThreadType = TYPE_TOP;
	for (int i = 0; i < 5; i++)
		m_aRankThreadResult[i][0] = '\0';
	void *pt = thread_init(TopThread, this);
	if (!pt)
		SendChatTarget(ClientID, "[stats] failed to spawn thread.");
}

void CGameContext::ShowRank(int ClientID, const char *pName)
{
	if (m_RankThreadState != RT_IDLE)
	{
		SendChatTarget(ClientID, "[stats] rank is currently being requested try agian later.");
		return;
	}
	m_RankThreadState = RT_ACTIVE;
	char aName[64];
	str_copy(aName, pName, sizeof(aName));
	str_clean_whitespaces_simple(aName);

	m_RankThreadTop = 0;
	m_RankThreadReqID = ClientID;
	m_RankThreadType = TYPE_RANK;
	str_copy(m_aRankThreadResult[0], "[stats] something went wrong.", sizeof(m_aRankThreadResult[0]));
	str_copy(m_aRankThreadName, aName, sizeof(m_aRankThreadName));
	str_copy(m_aRankThreadRequestName, Server()->ClientName(ClientID), sizeof(m_aRankThreadRequestName));
	void *pt = thread_init(RankThread, this);
	if (!pt)
		SendChatTarget(ClientID, "[stats] failed to spawn thread.");
}

int CGameContext::CalcScore(const CFngStats *pStats)
{
	int Score = 0;
	Score += pStats->m_Freezes;
	Score += pStats->m_Kills * 3;
	Score += pStats->m_GoldSpikes * 5;
	Score += pStats->m_GreenSpikes * 3;
	Score += pStats->m_PurpleSpikes * 7;
	return Score;
}

void CGameContext::MergeStats(const CFngStats *pFrom, CFngStats *pTo)
{
	str_copy(pTo->m_aName, pFrom->m_aName, sizeof(pTo->m_aName));
	str_copy(pTo->m_aClan, pFrom->m_aClan, sizeof(pTo->m_aClan));
	pTo->m_Kills += pFrom->m_Kills;
	pTo->m_Deaths += pFrom->m_Deaths;
	pTo->m_GoldSpikes += pFrom->m_GoldSpikes;
	pTo->m_GreenSpikes += pFrom->m_GreenSpikes;
	pTo->m_PurpleSpikes += pFrom->m_PurpleSpikes;
	pTo->m_RifleShots += pFrom->m_RifleShots;
	pTo->m_Freezes += pFrom->m_Freezes;
	pTo->m_Frozen += pFrom->m_Frozen;
	pTo->m_Spree = pFrom->m_Spree;
	pTo->m_SpreeBest = maximum(pTo->m_SpreeBest, pFrom->m_SpreeBest);
	pTo->m_Multi = pFrom->m_Multi;
	pTo->m_MultiBest = maximum(pTo->m_MultiBest, pFrom->m_MultiBest);
	for (int i = 0; i < MAX_MULTIS; i++)
		pTo->m_aMultis[i] += pFrom->m_aMultis[i];
	pTo->m_CfgFlags = pFrom->m_CfgFlags;
	pTo->m_LastSeen = maximum(pTo->m_LastSeen, pFrom->m_LastSeen);
	pTo->m_TotalOnlineTime += pFrom->m_TotalOnlineTime;
}

bool CGameContext::SaveStats(int ClientID, bool Failed)
{
	if (!g_Config.m_SvStats)
		return false;
	CPlayer *pPlayer = m_apPlayers[ClientID];
	if (!pPlayer)
		return false;
	char aFilename[MAX_FILE_LEN];
	if (escape_filename(aFilename, sizeof(aFilename), Server()->ClientName(ClientID)))
	{
		SendChatTarget(ClientID,
			Failed ?
			"[stats] save failed: escape error." :
			"[stats] recover save failed: escape error." // should be impossible to reach
		);
		return false;
	}
	char aFilePath[2048];
	str_format(aFilePath, sizeof(aFilePath), "%s/%s.acc", Failed ? g_Config.m_SvStatsFailPath : g_Config.m_SvStatsPath, aFilename);
	return pPlayer->SaveStats(aFilePath, Failed);
}

bool CGameContext::IsFngMagic(const char *pMagic, int Size)
{
	if (Size < FNG_MAGIC_LEN)
		return false;
	for (int i = 0; i < FNG_MAGIC_LEN; i++)
	{
		if (FNG_MAGIC[i] != pMagic[i])
			return false;
	}
	return true;
}

bool CGameContext::IsFngVersion(const char *pVersion, int Size)
{
	if (Size < FNG_VERSION_LEN)
		return false;
	for (int i = 0; i < FNG_VERSION_LEN; i++)
	{
		if (FNG_VERSION[i] != pVersion[i])
			return false;
	}
	return true;
}

int CGameContext::LoadStats(int ClientID, const char *pName, CFngStats *pStatsBuf)
{
	if (!g_Config.m_SvStats)
		return -1;
	char aFilename[MAX_FILE_LEN];
	if (escape_filename(aFilename, sizeof(aFilename), pName))
	{
		if (ClientID != -1)
			SendChatTarget(ClientID, "[stats] load failed: escape error.");
		return -2;
	}
	char aFilePath[MAX_FILE_PATH];
	str_format(aFilePath, sizeof(aFilePath), "%s/%s.acc", g_Config.m_SvStatsPath, aFilename);
	return LoadStatsFile(ClientID, aFilePath, pStatsBuf);
}

int CGameContext::LoadStatsFile(int ClientID, const char *pPath, CFngStats *pStatsBuf)
{
	if (!g_Config.m_SvStats)
		return -1;
	int err = 0;
	FILE *pFile;
	pFile = fopen(pPath, "rb");
	if (!pFile)
	{
		dbg_msg("load", "failed to load file '%s'", pPath); // TODO: remove
		err = 1; goto fail;
	}
	char aMagic[4];
	if (!fread(&aMagic, sizeof(aMagic), 1, pFile))
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "magic='%s'", aMagic);
		if (ClientID != -1)
			SendChatTarget(ClientID, aBuf);
		err = 2; goto fail;
	}
	if (!IsFngMagic(aMagic, sizeof(aMagic)))
	{
		dbg_msg("stats", "file error magic missmatch '%s' != 'FNG'", aMagic);
		err = 3; goto fail;
	}
	char aVersion[16];
	str_copy(aVersion, FNG_VERSION, sizeof(aVersion));
	if (!fread(&aVersion, sizeof(aVersion), 1, pFile))
	{
		err = 4; goto fail;
	}
	if (!IsFngVersion(aVersion, sizeof(aVersion)))
	{
		dbg_msg("stats", "file error version missmatch '%s' != '%s'", aVersion, FNG_VERSION);
		err = 5; goto fail;
	}
	if (!fread(pStatsBuf, sizeof(*pStatsBuf), 1, pFile))
	{
		err = 6; goto fail;
	}
	fclose(pFile);
	return 0;
	fail:
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "[stats] load failed: file error=%d path='%s'", err, pPath);
	if (err != 1 && ClientID != -1) // expected error when stats do not exist yet
		SendChatTarget(ClientID, aBuf);
	return err;
}

int CGameContext::TestSavePath(const char *pPath)
{
	FILE *pFile;
	pFile = fopen(pPath, "wb");
	if (!pFile)
		return 1;
	char x = 'x';
	if (!fwrite(&x, sizeof(x), 1, pFile))
		return 2;
	fclose(pFile);
	pFile = fopen(pPath, "rb");
	if (!pFile)
		return 3;
	if (!fread(&x, sizeof(x), 1, pFile))
		return 4;
	fclose(pFile);
	if (x != 'x')
		return 5;
	if (remove(pPath))
		return 6;
	return 0;
}

int CGameContext::TestSaveStats()
{
	char aPath[1024];
	int err = 0;
	str_format(aPath, sizeof(aPath), "%s/%s", g_Config.m_SvStatsPath, "remove_me.test");
	err = TestSavePath(aPath);
	if (err)
	{
		dbg_msg("solofng", "test stats path failed with err=%d path='%s'", err, aPath);
		return err;
	}
	str_format(aPath, sizeof(aPath), "%s/%s", g_Config.m_SvStatsFailPath, "remove_me.test");
	err = TestSavePath(aPath);
	if (err)
	{
		dbg_msg("solofng", "test stats path failed with err=%d path='%s'", err, aPath);
		return err;
	}
	return 0;
}

void CGameContext::MergeFailedStats(int ClientID)
{
	DIR *pDir;
	struct dirent *pDe;
	int total = 0;
	int merged = 0;
	pDir = opendir(g_Config.m_SvStatsFailPath);
	if(pDir == NULL)
	{
		dbg_msg("merge_stats", "failed to open directory '%s'.", g_Config.m_SvStatsFailPath);
		SendChatTarget(ClientID, "[stats] failed to open directory.");
		return;
	}
	while ((pDe = readdir(pDir)) != NULL)
	{
		if (str_endswith(pDe->d_name, ".lck"))
		{
			dbg_msg("merge_stats", "file '%s' is locked by write.", pDe->d_name);
			// err = 1; goto end;
			continue;
		}
		if (!str_comp(pDe->d_name, ".") || !str_comp(pDe->d_name, ".."))
		{
			// printf("ignore '%s'.\n", pDe->d_name);
			continue;
		}
		if (!str_endswith(pDe->d_name, ".acc"))
		{
			// printf("warning not a .acc file '%s'.\n", pDe->d_name);
			continue;
		}
		total++;
		char aFilePath[2048];
		char aSavePath[2048];
		str_format(aFilePath, sizeof(aFilePath), "%s/%s", g_Config.m_SvStatsFailPath, pDe->d_name);
		str_format(aSavePath, sizeof(aSavePath), "%s/%s", g_Config.m_SvStatsPath, pDe->d_name);
		// printf("load stats '%s' '%s' ... \n", pDe->d_name, aFilePath);
		CFngStats Stats;
		int load = LoadStatsFile(-1, aFilePath, &Stats);
		if (load)
		{
			dbg_msg("merge_stats", "file '%s' failed to load with err=%d", aFilePath, load);
			// err = 1; goto end;
			continue;
		}
		CFngStats *pMergeStats;
		CFngStats FileStats;
		bool HasStast = LoadStatsFile(-1, aSavePath, &FileStats) == 0;
		if (!HasStast)
		{
			// just copy failed stats over
			pMergeStats = &Stats;
		}
		else
		{
			MergeStats(&Stats, &FileStats);
			pMergeStats = &FileStats;
		}

		FILE *pFile;
	#if defined(CONF_FAMILY_UNIX)
		int fd;
		struct stat st0, st1;
		pFile = fopen(aSavePath, "wb");
		char aLockPath[2048+4];
		str_format(aLockPath, sizeof(aLockPath), "%s.lck", aSavePath);
		int trys = 0;
		int max_trys = 16;
		// lock file code by user2769258 and Arnaud Le Blanc
		// https://stackoverflow.com/a/18745264
		// Not portable! E.g. on Windows st_ino is always 0. – rustyx Nov 8 '17 at 18:35
		// Windows has own lock system (note by ChillerDragon)
		while(1)
		{
			trys++;
			fd = open(aLockPath, O_CREAT, S_IRUSR|S_IWUSR);
			flock(fd, LOCK_EX);

			fstat(fd, &st0);
			stat(aLockPath, &st1);
			if(st0.st_ino == st1.st_ino) break;

			dbg_msg("stats_merge", "wait for locked file %d/%d...", trys, max_trys);
			close(fd);
			if (trys > max_trys)
			{
				pFile = NULL;
				dbg_msg("stats_merge", "save failed: file locked '%s'", aSavePath);
				continue;
			}
		}
	#endif
		if(!pFile)
		{
			dbg_msg("stats_merge", "save failed: file open '%s'", aSavePath);
			continue;
		}
		fwrite(&FNG_MAGIC, sizeof(FNG_MAGIC), 1, pFile);
		fwrite(&FNG_VERSION, sizeof(FNG_VERSION), 1, pFile);
		fwrite(pMergeStats, sizeof(*pMergeStats), 1, pFile);
		fclose(pFile);
	#if defined(CONF_FAMILY_UNIX)
		unlink(aLockPath);
		flock(fd, LOCK_UN);
	#endif
		merged++;
		dbg_msg("merge_stats", "saved stats to file '%s' (%s)", aSavePath, HasStast ? "merge" : "new");
		if (remove(aFilePath))
		{
			dbg_msg("merge_stats", "error: failed to remove stats file! '%s'", aFilePath);
			exit(1);
		}
	}
	closedir(pDir);
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "[stats] merged %d/%d failed stats to main database.", merged, total);
	SendChatTarget(ClientID, aBuf);
}

void CGameContext::ChatCommand(int ClientID, const char *pFullCmd)
{
	dbg_msg("chat_cmd", "ClientID=%d executed '/%s'", ClientID, pFullCmd);
	char aBuf[1024];
	CPlayer *pPlayer = m_apPlayers[ClientID];
	if (!pPlayer)
		return;
	if(!str_comp_nocase(pFullCmd, "help") || !str_comp_nocase(pFullCmd, "info"))
	{
		str_format(aBuf, sizeof(aBuf), "solofng by ChillerDragon - v%s", FNG_VERSION);
		SendChatTarget(ClientID, aBuf);
		SendChatTarget(ClientID, "https://github.com/zillyfng/solofng");
	}
	else if(!str_comp_nocase("cmdlist", pFullCmd))
	{
		SendChatTarget(ClientID, "commands: stats, top5, round, cmdlist, help, info, config");
		if (Server()->IsAuthed(ClientID))
			SendChatTarget(ClientID, "admin: meta, save, admin, merge_failed");
	}
	else if(!str_comp_nocase("stats", pFullCmd))
	{
		if (!g_Config.m_SvStats)
		{
			SendChatTarget(ClientID, "[stats] deactivated by admin.");
			return;
		}
		ShowStats(ClientID, Server()->ClientName(ClientID));
	}
	else if(!str_comp_nocase_num("stats ", pFullCmd, 6))
	{
		if (!g_Config.m_SvStats)
		{
			SendChatTarget(ClientID, "[stats] deactivated by admin.");
			return;
		}
		ShowStats(ClientID, pFullCmd+6);
	}
	else if(!str_comp_nocase("top5", pFullCmd))
	{
		if (!g_Config.m_SvStats || !g_Config.m_SvAllowRankCmds)
		{
			SendChatTarget(ClientID, "[stats] deactivated by admin.");
			return;
		}
		ShowTopScore(ClientID);
	}
	else if(!str_comp_nocase_num("top5 ", pFullCmd, 5))
	{
		if (!g_Config.m_SvStats || !g_Config.m_SvAllowRankCmds)
		{
			SendChatTarget(ClientID, "[stats] deactivated by admin.");
			return;
		}
		int Top = atoi(pFullCmd+5);
		ShowTopScore(ClientID, Top);
	}
	else if(!str_comp_nocase("rank", pFullCmd))
	{
		if (!g_Config.m_SvStats || !g_Config.m_SvAllowRankCmds)
		{
			SendChatTarget(ClientID, "[stats] deactivated by admin.");
			return;
		}
		ShowRank(ClientID, Server()->ClientName(ClientID));
	}
	else if(!str_comp_nocase_num("rank ", pFullCmd, 5))
	{
		if (!g_Config.m_SvStats || !g_Config.m_SvAllowRankCmds)
		{
			SendChatTarget(ClientID, "[stats] deactivated by admin.");
			return;
		}
		ShowRank(ClientID, pFullCmd+5);
	}
	else if(!str_comp_nocase("save", pFullCmd))
	{
		if (!g_Config.m_SvStats)
		{
			SendChatTarget(ClientID, "[stats] deactivated by admin.");
			return;
		}
		if (Server()->IsAuthedAdmin(ClientID))
			SaveStats(ClientID);
		else
			SendChatTarget(ClientID, "missing permission.");
	}
	else if (!str_comp_nocase_num("round ", pFullCmd, 6))
	{
		ShowRoundStats(ClientID, pFullCmd+6);
	}
	else if (!str_comp_nocase("round", pFullCmd))
	{
		ShowRoundStats(ClientID, Server()->ClientName(ClientID));
	}
	else if(!str_comp_nocase_num("meta ", pFullCmd, 5))
	{
		if (Server()->IsAuthed(ClientID))
			ShowStatsMeta(ClientID, pFullCmd+5);
		else
			SendChatTarget(ClientID, "missing permission.");
	}
	else if(!str_comp_nocase("list", pFullCmd))
	{
		str_format(aBuf, sizeof(aBuf), "Online: %d Ingame: %d", CountPlayers(), CountIngamePlayers());
		SendChatTarget(ClientID, aBuf);
	}
	else if(!str_comp_nocase("config", pFullCmd))
	{
		SendChatTarget(ClientID, "Usage: /config <config> <value>");
		SendChatTarget(ClientID, "Configs: hammer");
		SendChatTarget(ClientID, "-	Values: fng, vanilla");
		SendChatTarget(ClientID, "Example: /config hammer vanilla");
	}
	else if(!str_comp_nocase_num("config ", pFullCmd, 7))
	{
		pPlayer->InitRoundStats();
		char aCfg[16];
		str_copy(aCfg, pFullCmd+7, sizeof(aCfg));
		if(!str_comp_nocase("hammer", aCfg))
		{
			str_format(aBuf, sizeof(aBuf), "[config] hammer tune is set to '%s'", pPlayer->IsConfig(CFG_VANILLA_HAMMER) ? "vanilla" : "fng");
			SendChatTarget(ClientID, aBuf);
		}
		else if(!str_comp_nocase_num("hammer ", aCfg, 7))
		{
			char aValue[16];
			str_copy(aValue, aCfg+7, sizeof(aValue));
			if (!str_comp_nocase(aValue, "fng"))
			{
				SendChatTarget(ClientID, "[config] update hammer tune to 'fng'.");
				pPlayer->UnsetConfig(CFG_VANILLA_HAMMER);
			}
			else if (!str_comp_nocase(aValue, "vanilla"))
			{
				SendChatTarget(ClientID, "[config] update hammer tune to 'vanilla'.");
				pPlayer->SetConfig(CFG_VANILLA_HAMMER);
			}
			else
			{
				SendChatTarget(ClientID, "[config] invalid value use of those: fng, vanilla");
			}
		}
		else
		{
			SendChatTarget(ClientID, "[config] invalid config use one of those: hammer");
		}
	}
	else if(!str_comp_nocase("admin", pFullCmd))
	{
		if (!Server()->IsAuthedAdmin(ClientID))
		{
			SendChatTarget(ClientID, "missing permission.");
			return;
		}
		if (m_StatSaveFails || m_StatSaveCriticalFails)
		{
			str_format(aBuf, sizeof(aBuf), "[stats] fails: %d critical fails: %d", m_StatSaveFails, m_StatSaveCriticalFails);
			SendChatTarget(ClientID, aBuf);
		}
		else
		{
			SendChatTarget(ClientID, "[stats] all saves successful.");
		}
	}
	else if(!str_comp_nocase("merge_failed", pFullCmd))
	{
		if (!Server()->IsAuthedAdmin(ClientID))
		{
			SendChatTarget(ClientID, "missing permission.");
			return;
		}
		MergeFailedStats(ClientID);
	}
#ifdef CONF_DEBUG
	else if(!str_comp_nocase("crash", pFullCmd))
	{
		if (!Server()->IsAuthedAdmin(ClientID))
		{
			SendChatTarget(ClientID, "missing permission.");
			return;
		}
		for (int i = 0; i < MAX_CLIENTS; i++)
		{
			m_apPlayers[i]->GetCharacter()->m_DeepFreeze = 2;
		}
	}
#endif
	else
	{
		SendChatTarget(ClientID, "unknown command try '/cmdlist'.");
	}
}

int CGameContext::GetCIDByName(const char *pName)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		if (!m_apPlayers[i])
			continue;
		if (!str_comp(pName, Server()->ClientName(i)))
			return i;
	}
	return -1;
}

int CGameContext::CountPlayers()
{
	int c = 0;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (m_apPlayers[i])
			c++;
	return c;
}

int CGameContext::CountIngamePlayers()
{
	int c = 0;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (m_apPlayers[i] && m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
			c++;
	return c;
}

void CGameContext::EndRound()
{
	dbg_msg("solofng", "round end saving all stats...");
	if (!g_Config.m_SvStats)
		return;
	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		if (!m_apPlayers[i])
			continue;
		SaveStats(i);
	}
}

const char *CGameContext::GameType() const { return m_pController && m_pController->GetGameType() ? m_pController->GetGameType() : ""; }
const char *CGameContext::Version() const { return GAME_VERSION; }
const char *CGameContext::NetVersion() const { return GAME_NETVERSION; }
const char *CGameContext::NetVersionHashUsed() const { return GAME_NETVERSION_HASH_FORCED; }
const char *CGameContext::NetVersionHashReal() const { return GAME_NETVERSION_HASH; }

IGameServer *CreateGameServer() { return new CGameContext; }
