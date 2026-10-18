// ----------------------------------------------------------------------- //
//
// MODULE  : ScreenHostDMOptions.cpp
//
// PURPOSE : Interface screen for hosting multi player games
//
// (c) 1999-2002 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "ClientMultiplayerMgr.h"
#include "ScreenHostDMOptions.h"
#include "ScreenMgr.h"
#include "ScreenCommands.h"
#include "GameClientShell.h"
#include "MsgIDs.h"
#include "ResShared.h"

namespace
{
	const uint32 CMD_SPAWN_BOT = CMD_CUSTOM+10;
	const uint32 CMD_REMOVE_BOT = CMD_CUSTOM+11;
	// Bots are controlled through the BotCount / BotSkill console variables,
	// which the server (listen server) reads every half second.
	int GetBotVar( const char* szName, int nDefault )
	{
		HCONSOLEVAR hVar = g_pLTClient->GetConsoleVar( szName );
		return hVar ? ( int )g_pLTClient->GetVarValueFloat( hVar ) : nDefault;
	}
	void SetBotVar( const char* szName, int nValue )
	{
		char szCmd[64];
		sprintf( szCmd, "%s %d", szName, nValue );
		g_pLTClient->RunConsoleString( szCmd );
	}
	const int kMaxScoreLimit = 100;
	const int kMaxTimeLimit = 60;
	const int kMaxRunSpeed = 150;
	const int kMaxRounds = 20;
}


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CScreenHostDMOptions::CScreenHostDMOptions()
{
	m_nMaxPlayers = 8;
	m_nRunSpeed = 130;
	m_nScoreLimit = 25;
	m_nTimeLimit = 10;
	m_nRounds = 1;
	m_nBots = 0;
	m_nBotSkill = 2;
	m_nBotSkillIdx = 0;
	m_nBotsApplied = 0;
	m_nSkillIdxApplied = 0;
	m_pSpawnBot = LTNULL;
	m_pRemoveBot = LTNULL;

	m_nFragScore = 1;

	m_pMaxPlayers = NULL;

	m_bWeaponsStay = false;
}


CScreenHostDMOptions::~CScreenHostDMOptions()
{
}

// Build the screen
LTBOOL CScreenHostDMOptions::Build()
{
	int kColumn = g_pLayoutMgr->GetScreenCustomInt(SCREEN_ID_HOST_DM_OPTIONS,"ColumnWidth");
	int kSlider = g_pLayoutMgr->GetScreenCustomInt(SCREEN_ID_HOST_DM_OPTIONS,"SliderWidth");

	CreateTitle(IDS_TITLE_HOST_OPTIONS);

	m_pMaxPlayers = AddSlider(IDS_MAX_PLAYERS, IDS_MAX_PLAYERS_HELP, kColumn, kSlider, -1, &m_nMaxPlayers);
	m_pMaxPlayers->SetSliderRange(2, 16);
	m_pMaxPlayers->SetSliderIncrement(1);
	m_pMaxPlayers->SetNumericDisplay(LTTRUE);


	char szYes[16];
	char szNo[16];
	FormatString(IDS_YES,szYes,sizeof(szYes));
	FormatString(IDS_NO,szNo,sizeof(szNo));

	CLTGUISlider*	pSlider = AddSlider(IDS_RUN_SPEED, IDS_RUN_SPEED_HELP, kColumn, kSlider, -1, &m_nRunSpeed);
	pSlider->SetSliderRange(100, kMaxRunSpeed);
	pSlider->SetSliderIncrement(10);
	pSlider->SetNumericDisplay(LTTRUE);

	pSlider = AddSlider(IDS_FRAG_LIMIT, IDS_FRAG_LIMIT_HELP, kColumn, kSlider, -1, &m_nScoreLimit);
	pSlider->SetSliderRange(0,kMaxScoreLimit);
	pSlider->SetSliderIncrement(5);
	pSlider->SetNumericDisplay(LTTRUE);

	pSlider = AddSlider(IDS_TIME_LIMIT, IDS_TIME_LIMIT_HELP, kColumn, kSlider, -1, &m_nTimeLimit);
	pSlider->SetSliderRange(0,kMaxTimeLimit);
	pSlider->SetSliderIncrement(5);
	pSlider->SetNumericDisplay(LTTRUE);

	pSlider = AddSlider( IDS_ROUNDS, IDS_ROUNDS_HELP, kColumn, kSlider, -1, &m_nRounds );
	pSlider->SetSliderRange( 1, kMaxRounds );
	pSlider->SetSliderIncrement( 1 );
	pSlider->SetNumericDisplay( LTTRUE );

//	pSlider = AddSlider(IDS_FRAG_SCORE, IDS_FRAG_SCORE_HELP, kColumn, kSlider, -1, &m_nFragScore);
//	pSlider->SetSliderRange(0,3);
//	pSlider->SetSliderIncrement(1);
//	pSlider->SetNumericDisplay(LTTRUE);

	CLTGUIToggle* pToggle = NULL;
	pToggle = AddToggle(IDS_WEAPONSSTAY, IDS_WEAPONSSTAY_HELP, kColumn,&m_bWeaponsStay);
	pToggle->SetOnString(szYes);
	pToggle->SetOffString(szNo);

	// Computer-controlled players.
	char szBots[] = "Bots";
	char szBotSkill[] = "Bot Skill";
	char szSkilled[] = "Normal";
	char szHard[] = "Hard";
	char szExpert[] = "Expert";
	char szSpawn[] = "Spawn Bot";
	char szRemove[] = "Remove Bot";

	CLTGUICycleCtrl* pSkillCycle = AddCycle(szBotSkill, 0, kColumn, &m_nBotSkillIdx);
	pSkillCycle->AddString(szSkilled);
	pSkillCycle->AddString(szHard);
	pSkillCycle->AddString(szExpert);

	m_pSpawnBot = AddTextItem(szSpawn, CMD_SPAWN_BOT, 0);

	// Make sure to call the base class
	return CBaseScreen::Build();
}

// Change in focus
void    CScreenHostDMOptions::OnFocus(LTBOOL bFocus)
{
	CUserProfile *pProfile = g_pProfileMgr->GetCurrentProfile();
		
	if (bFocus)
	{

		m_nMaxPlayers = (int)pProfile->m_ServerGameOptions.GetDeathmatch().m_nMaxPlayers;
		m_nRunSpeed = (int)pProfile->m_ServerGameOptions.GetDeathmatch().m_nRunSpeed;
		m_nScoreLimit = (int)pProfile->m_ServerGameOptions.GetDeathmatch().m_nScoreLimit;
		m_nTimeLimit = (int)pProfile->m_ServerGameOptions.GetDeathmatch().m_nTimeLimit;
		m_nRounds = (int)pProfile->m_ServerGameOptions.GetDeathmatch().m_nRounds;

//		m_nFragScore = (int)pProfile->m_ServerGameOptions.GetDeathmatch().m_nFragScore;
		
		m_pMaxPlayers->Enable(!g_pPlayerMgr->IsPlayerInWorld());

		m_bWeaponsStay = pProfile->m_ServerGameOptions.GetDeathmatch().m_bWeaponsStay;

		m_nBots = GetBotVar("BotCount", 0);
		m_nBotSkill = GetBotVar("BotSkill", 2);
		if( m_nBotSkill < 2 ) m_nBotSkill = 2;
		if( m_nBotSkill > 4 ) m_nBotSkill = 4;
		m_nBotSkillIdx = ( uint8 )( m_nBotSkill - 2 );
		m_nBotsApplied = m_nBots;
		m_nSkillIdxApplied = m_nBotSkillIdx;

		// Spawn / Remove act on a running game you host; hide them otherwise.
		bool bLiveHost = g_pLTClient->IsConnected( ) && !g_pClientMultiplayerMgr->IsConnectedToRemoteServer( );
		if( m_pSpawnBot ) m_pSpawnBot->Show( bLiveHost );
		if( m_pRemoveBot ) m_pRemoveBot->Show( bLiveHost );

		UpdateData(LTFALSE);

	}
	else
	{
		UpdateData();

		pProfile->m_ServerGameOptions.GetDeathmatch().m_nMaxPlayers = (uint8)m_nMaxPlayers;
		pProfile->m_ServerGameOptions.GetDeathmatch().m_nRunSpeed = (uint8)m_nRunSpeed;
		pProfile->m_ServerGameOptions.GetDeathmatch().m_nScoreLimit = (uint8)m_nScoreLimit;
		pProfile->m_ServerGameOptions.GetDeathmatch().m_nTimeLimit = (uint8)m_nTimeLimit;
		pProfile->m_ServerGameOptions.GetDeathmatch().m_nRounds = (uint8)m_nRounds;

//		pProfile->m_ServerGameOptions.GetDeathmatch().m_nFragScore = (uint8)m_nFragScore;
		
		pProfile->m_ServerGameOptions.GetDeathmatch().m_bWeaponsStay = !!m_bWeaponsStay;

		SetBotVar("BotCount", m_nBots);
		SetBotVar("BotSkill", m_nBotSkillIdx + 2);

		// Spawned bots take the chosen difficulty (sent with each Spawn Bot).

		pProfile->Save();

		if (g_pPlayerMgr->IsPlayerInWorld())
		{

			CAutoMessage cMsg;
			cMsg.Writeuint8( MID_MULTIPLAYER_OPTIONS );
			pProfile->m_ServerGameOptions.GetDeathmatch().Write(cMsg);
		    g_pLTClient->SendToServer(cMsg.Read(), MESSAGE_GUARANTEED);
		}

	}
	CBaseScreen::OnFocus(bFocus);
}

uint32 CScreenHostDMOptions::OnCommand(uint32 dwCommand, uint32 dwParam1, uint32 dwParam2)
{
	switch(dwCommand)
	{
	case CMD_SPAWN_BOT:
	case CMD_REMOVE_BOT:
		{
			// Host only, in a running game.  Adds / removes one bot right now
			// without touching the Bots setting.
			if( !g_pLTClient->IsConnected( ) || g_pClientMultiplayerMgr->IsConnectedToRemoteServer( ))
				break;
			UpdateData();
			char szCmd[64];
			if( dwCommand == CMD_SPAWN_BOT )
				sprintf( szCmd, "bot-add 1 %d", m_nBotSkillIdx + 2 );
			else
				sprintf( szCmd, "bot-remove" );
			g_pLTClient->RunConsoleString( szCmd );
		} break;

	default:
		return CBaseScreen::OnCommand(dwCommand,dwParam1,dwParam2);
	}
	return 1;
}
