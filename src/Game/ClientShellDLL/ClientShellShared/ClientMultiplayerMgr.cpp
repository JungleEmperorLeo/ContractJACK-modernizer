// ----------------------------------------------------------------------- //
//
// MODULE  : ClientMultiplayerMgr.cpp
//
// PURPOSE : Clientside multiplayer mgr - Definition
//
// CREATED : 02/05/02
//
// (c) 1997-2002 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "ClientMultiplayerMgr.h"
#include "GameClientShell.h"
#include "MsgIds.h"
#include "CharacterFx.h"
#include "CMoveMgr.h"
#include "MissionMgr.h"
#include "MissionButeMgr.h"
#include "InterfaceMgr.h"
#include "ProfileMgr.h"
#include "WeaponMgr.h"
#include "CRC32.h"
#include "IGameSpy.h"


#pragma MESSAGE( "FIXFIX:  Should really be called ClientConnectionMgr." )


ClientMultiplayerMgr* g_pClientMultiplayerMgr = NULL;

inline unsigned char INADDR_B1(const sockaddr_in& addr)
{ return addr.sin_addr.S_un.S_un_b.s_b1; }
inline unsigned char INADDR_B2(const sockaddr_in& addr)
{ return addr.sin_addr.S_un.S_un_b.s_b2; }
inline unsigned char INADDR_B3(const sockaddr_in& addr)
{ return addr.sin_addr.S_un.S_un_b.s_b3; }
inline unsigned char INADDR_B4(const sockaddr_in& addr)
{ return addr.sin_addr.S_un.S_un_b.s_b4; }
#define EXPAND_BASEADDR(addr)\
              INADDR_B1(addr), INADDR_B2(addr), INADDR_B3(addr), INADDR_B4(addr)

#define EXPAND_ADDR(addr) \
	EXPAND_BASEADDR(addr),\
	ntohs((addr).sin_port)

static const char* ADDR_PRINTF = "%d.%d.%d.%d:%d";


// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::ClientMultiplayerMgr
//
//	PURPOSE:	Constructor
//
// --------------------------------------------------------------------------- //

ClientMultiplayerMgr::ClientMultiplayerMgr( )
{
	m_nServerPort = -1;
	m_nServerKey = 0;
	m_nDisconnectCode = 0;
	m_bForceDisconnect = false;
//	m_pServerDir = 0;
	m_aCurMessageSourceAddr[0] = 0;
	m_aCurMessageSourceAddr[1] = 0;
	m_aCurMessageSourceAddr[2] = 0;
	m_aCurMessageSourceAddr[3] = 0;
	m_nCurMessageSourcePort = 0;

	m_StartGameRequest.m_Type = GAMEMODE_NONE;
	memset( &m_NetClientData, 0, sizeof( m_NetClientData ));

	m_nLastConnectionResult = LT_OK;

	g_pClientMultiplayerMgr = this;

	m_nTeam = INVALID_TEAM;
	m_bHasSelectedTeam = false;

	m_pRetailGameSpyBrowser	= NULL;
	m_pDemoGameSpyBrowser = NULL;
	m_bDoNatNegotiations = false;
	m_bConnectViaPublic = true;

	m_eConnectionState = eConnectionState_Disconnected;
}

// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::~ClientMultiplayerMgr
//
//	PURPOSE:	Destructor
//
// --------------------------------------------------------------------------- //

ClientMultiplayerMgr::~ClientMultiplayerMgr( )
{
	TermBrowsers( );

	g_pClientMultiplayerMgr = NULL;
}

// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::InitSinglePlayer
//
//	PURPOSE:	Send the server the initial single player info
//
// --------------------------------------------------------------------------- //

bool ClientMultiplayerMgr::InitSinglePlayer()
{

	g_pWeaponMgr->LoadOverrideButes( WEAPON_DEFAULT_FILE );

	//force server to update difficulty
	GameDifficulty eDiff = g_pGameClientShell->GetDifficulty();
	g_pGameClientShell->SetDifficulty(eDiff);

	//force server to update performance settings
	CUserProfile *pProfile = g_pProfileMgr->GetCurrentProfile();
	pProfile->SendPerformanceMsg();


	return true;
}



// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::InitMultiPlayer
//
//	PURPOSE:	Send the server the initial multiplayer info
//
// --------------------------------------------------------------------------- //

bool ClientMultiplayerMgr::InitMultiPlayer()
{
	if (!IsMultiplayerGame())
		return false;
	CUserProfile *pProfile = g_pProfileMgr->GetCurrentProfile();
	ModelId modelId = eModelIdInvalid;	

	switch (g_pGameClientShell->GetGameType())
	{
	case eGameTypeCooperative:
		if (pProfile->m_nCPPlayerModel >= g_pModelButeMgr->GetNumCPModels())
			pProfile->m_nCPPlayerModel = 0;

		modelId = g_pModelButeMgr->GetCPModel(pProfile->m_nCPPlayerModel);
		break;
	case eGameTypeDeathmatch:
		if (pProfile->m_nDMPlayerModel >= g_pModelButeMgr->GetNumDMModels())
			pProfile->m_nDMPlayerModel = 0;

		modelId = g_pModelButeMgr->GetDMModel(pProfile->m_nDMPlayerModel);
		break;
//		if (pProfile->m_nDMPlayerModel >= g_pModelButeMgr->GetNumDMModels())
//			pProfile->m_nDMPlayerModel = 0;
//
//		modelId = g_pModelButeMgr->GetDMModel(pProfile->m_nDMPlayerModel);
		break;
	};

	CAutoMessage cMsg;
  
	cMsg.Writeuint8( MID_PLAYER_MULTIPLAYER_INIT );

	ASSERT(( uint8 )modelId == modelId );
    cMsg.Writeuint8(( uint8 )modelId );
	cMsg.Writeuint8( m_nTeam );

	for (uint8 i = 0; i < kNumSkills; i++)
	{
		cMsg.Writeuint8(pProfile->m_nPlayerSkills[i]);
	}


    g_pLTClient->SendToServer(cMsg.Read(), MESSAGE_GUARANTEED);

	//force server to update difficulty

	GameDifficulty eDiff = GD_NORMAL;
	if (g_pGameClientShell->GetGameType() == eGameTypeCooperative)
	{
		eDiff = (GameDifficulty)pProfile->m_ServerGameOptions.GetCoop().m_nDifficulty;
	}
	g_pGameClientShell->SetDifficulty(eDiff);

	//force server to update performance settings
	pProfile->SendPerformanceMsg();



	return true;
}


// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::UpdateMultiPlayer
//
//	PURPOSE:	Send the server the updated multiplayer info
//
// --------------------------------------------------------------------------- //

bool ClientMultiplayerMgr::UpdateMultiPlayer()
{
	if (!IsMultiplayerGame())
		return false;

	CUserProfile *pProfile = g_pProfileMgr->GetCurrentProfile();
	ModelId modelId = eModelIdInvalid;	
	
	switch (g_pGameClientShell->GetGameType())
	{
	case eGameTypeCooperative:
		if (pProfile->m_nCPPlayerModel >= g_pModelButeMgr->GetNumCPModels())
			pProfile->m_nCPPlayerModel = 0;

		modelId = g_pModelButeMgr->GetCPModel(pProfile->m_nCPPlayerModel);
		break;
	case eGameTypeDeathmatch:
		if (pProfile->m_nDMPlayerModel >= g_pModelButeMgr->GetNumDMModels())
			pProfile->m_nDMPlayerModel = 0;

		modelId = g_pModelButeMgr->GetDMModel(pProfile->m_nDMPlayerModel);
		break;
	};

	if( !UpdateNetClientData( ))
		return false;

	CAutoMessage cMsg;
  
	cMsg.Writeuint8( MID_PLAYER_INFOCHANGE );

	cMsg.WriteString(pProfile->m_sPlayerName.c_str());

	ASSERT(( uint8 )modelId == modelId );
    cMsg.Writeuint8(( uint8 )modelId );
	cMsg.Writeuint8( m_nTeam );


    g_pLTClient->SendToServer(cMsg.Read(), MESSAGE_GUARANTEED);

	return true;
}

// returns true if the passed in address matches the current server address
bool ClientMultiplayerMgr::CheckServerAddress(char const*pszTestAddress, int nPort)
{
	if (!pszTestAddress) return false;
	if (nPort != m_nServerPort) return false;
	return (stricmp(pszTestAddress,m_sServerAddress) == 0);
}


void ClientMultiplayerMgr::DoTaunt(uint32 nClientID, uint32 nTauntID)
{
	//if you're not listening to taunts, you're not allowed to send them
	if (GetConsoleInt("IgnoreTaunts",0) > 0) return;

	CClientInfoMgr *pCIMgr = g_pGameClientShell->GetInterfaceMgr( )->GetClientInfoMgr();
	if (!pCIMgr) return;

//	char szVar[16] = "";
//	sprintf(szVar,"TauntDM%d",nTaunt);

//	uint32 nTauntID = (uint32)GetConsoleInt(szVar,0);

	if (!nTauntID) return;

	// Don't allow the client to flood the server with taunts...

	CCharacterFX *pFX = g_pGameClientShell->GetPlayerMgr( )->GetMoveMgr()->GetCharacterFX();
	if (pFX && !pFX->IsPlayingTaunt())
	{
		pFX->PlayTaunt(nTauntID);

		// create the message
		char strMessage[256];
		SAFE_STRCPY(strMessage, LoadTempString(nTauntID));

		CAutoMessage cMsg;
		cMsg.Writeuint8(MID_PLAYER_TAUNT);
		cMsg.Writeuint32(nTauntID);
	    cMsg.WriteString(strMessage);
	    g_pLTClient->SendToServer(cMsg.Read(), MESSAGE_GUARANTEED);
	}
}



bool ClientMultiplayerMgr::SetupClient(char const* pszHostName, char const* pszPassword,
							bool bDoNatNegotiations, bool bConnectViaPublic, 
							char const* pszPublicAddress, char const* pszPrivateAddress )
{
	// Check inputs.
	if( !pszPublicAddress || !pszPublicAddress[0] || !pszPrivateAddress || !pszPrivateAddress[0] )
	{
		ASSERT( !"ClientMultiplayerMgr::SetupClient: Invalid inputs." );
		return false;
	}

	m_sConnectPublicAddress = pszPublicAddress;
	m_sConnectPrivateAddress = pszPrivateAddress;
	m_bConnectViaPublic = bConnectViaPublic;

	// Setup the startgame info.
	StartGameRequest startGameRequest;
	m_StartGameRequest = startGameRequest;
	m_ServerGameOptions.Clear( );
	memset( &m_NetClientData, 0, sizeof( m_NetClientData ));

	if( !UpdateNetClientData( ))
		return false;

	// Record the default address to try.
	strncpy( m_StartGameRequest.m_TCPAddress, ( m_bConnectViaPublic ? m_sConnectPublicAddress.c_str( ) : 
		m_sConnectPrivateAddress.c_str( )), MAX_SGR_STRINGLEN);

	m_StartGameRequest.m_Type = STARTGAME_CLIENTTCP;
	if( pszHostName && pszHostName[0] )
	{
		SAFE_STRCPY(m_StartGameRequest.m_HostInfo.m_sName ,pszHostName);
	}

	if (pszPassword)
	{
		m_nClientPass = str_Hash(pszPassword);
	}
	else
	{
		m_nClientPass = 0;
	}

	// Check if they want us to do natnegotiations.
	m_bDoNatNegotiations = bDoNatNegotiations;

	return true;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::SetupServerSinglePlayer
//
//	PURPOSE:	Setup server for singleplayer game.
//
// ----------------------------------------------------------------------- //
bool ClientMultiplayerMgr::SetupServerSinglePlayer( )
{
	StartGameRequest startGameRequest;
	m_StartGameRequest = startGameRequest;
	m_ServerGameOptions.Clear( );
	memset( &m_NetClientData, 0, sizeof( m_NetClientData ));

	// Set the game type to normal (single player).
	m_StartGameRequest.m_Type = STARTGAME_NORMAL;

	CUserProfile *pProfile = g_pProfileMgr->GetCurrentProfile();
	m_ServerGameOptions = pProfile->m_ServerGameOptions;
	m_ServerGameOptions.m_bPreCacheAssets = (pProfile->m_sPerformance.nSettings[kPerform_PreCacheAssets] > 0);
	m_ServerGameOptions.m_bPerformanceTest = g_pGameClientShell->IsRunningPerformanceTest();

	return true;
}
// --------------------------------------------------------------------------- //
//
//  ROUTINE:    ClientMultiplayerMgr::SetupServerHost()
//
//  PURPOSE:    Host a game.
//
//	PARAMETERS:	int nPort - Port to use, 0 to use default
//
// --------------------------------------------------------------------------- //

bool ClientMultiplayerMgr::SetupServerHost( int nPort, bool bLANOnly )
{
	StartGameRequest startGameRequest;
	m_StartGameRequest = startGameRequest;
	m_ServerGameOptions.Clear( );
	memset( &m_NetClientData, 0, sizeof( m_NetClientData ));
	
	CUserProfile *pProfile = g_pProfileMgr->GetCurrentProfile();
	m_ServerGameOptions = pProfile->m_ServerGameOptions;
	m_ServerGameOptions.m_bLANOnly = pProfile->m_ServerGameOptions.m_bLANOnly = bLANOnly;
	m_ServerGameOptions.m_bPreCacheAssets = (pProfile->m_sPerformance.nSettings[kPerform_PreCacheAssets] > 0);

	if( !UpdateNetClientData( ))
		return false;

	m_StartGameRequest.m_Type = STARTGAME_HOST;
	m_StartGameRequest.m_HostInfo.m_Port = nPort;

	switch (g_pGameClientShell->GetGameType())
	{
	case eGameTypeCooperative:
	    m_StartGameRequest.m_HostInfo.m_dwMaxConnections = pProfile->m_ServerGameOptions.GetCoop().m_nMaxPlayers-1;
		LTStrCpy( m_StartGameRequest.m_HostInfo.m_sName, pProfile->m_ServerGameOptions.GetCoop().m_sSessionName.c_str( ),
					sizeof(m_StartGameRequest.m_HostInfo.m_sName));
		break;
	case eGameTypeDeathmatch:
	    m_StartGameRequest.m_HostInfo.m_dwMaxConnections = pProfile->m_ServerGameOptions.GetDeathmatch().m_nMaxPlayers-1;
		LTStrCpy( m_StartGameRequest.m_HostInfo.m_sName, pProfile->m_ServerGameOptions.GetDeathmatch().m_sSessionName.c_str( ),
					sizeof(m_StartGameRequest.m_HostInfo.m_sName));
		break;
	case eGameTypeTeamDeathmatch:
	    m_StartGameRequest.m_HostInfo.m_dwMaxConnections = pProfile->m_ServerGameOptions.GetTeamDeathmatch().m_nMaxPlayers-1;
		LTStrCpy( m_StartGameRequest.m_HostInfo.m_sName, pProfile->m_ServerGameOptions.GetTeamDeathmatch().m_sSessionName.c_str( ),
					sizeof(m_StartGameRequest.m_HostInfo.m_sName));
		break;
	case eGameTypeDemolition:
	    m_StartGameRequest.m_HostInfo.m_dwMaxConnections = pProfile->m_ServerGameOptions.GetDemolition().m_nMaxPlayers-1;
		LTStrCpy( m_StartGameRequest.m_HostInfo.m_sName, pProfile->m_ServerGameOptions.GetDemolition().m_sSessionName.c_str( ),
					sizeof(m_StartGameRequest.m_HostInfo.m_sName));
		break;
	case eGameTypeDoomsDay:
	    m_StartGameRequest.m_HostInfo.m_dwMaxConnections = pProfile->m_ServerGameOptions.GetDoomsday().m_nMaxPlayers-1;
		LTStrCpy( m_StartGameRequest.m_HostInfo.m_sName, pProfile->m_ServerGameOptions.GetDoomsday().m_sSessionName.c_str( ),
					sizeof(m_StartGameRequest.m_HostInfo.m_sName));
		break;
	};


	m_StartGameRequest.m_HostInfo.m_bHasPassword = pProfile->m_ServerGameOptions.m_bUsePassword;
	m_StartGameRequest.m_HostInfo.m_nGameType = (uint8)pProfile->m_ServerGameOptions.m_eGameType;
	
	// Set the name of the mod we are using...
	
	m_ServerGameOptions.m_sModName = GetModName();
	
/*
	// Make sure that the multiplayer mgr doesn't have a server directory in use
	// This must be done because there can only be one IServerDirectory object
	// created at a time for proper shutdown.  (Internal Titan implementation BS...)
	// NYI - Note : This shouldn't be necessary, and if it is, it will cause problems
	// elsewhere.  (Host/Join sequence = 2 objects) Figure out a way to get around this 
	// restriction.
	DeleteServerDir( );
*/
    return true;
}


// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::SetDisconnectCode
//
//	PURPOSE:	Sets the disconnection code and message
//
// --------------------------------------------------------------------------- //

void ClientMultiplayerMgr::SetDisconnectCode(uint32 nCode, const char *pMsg)
{
	// Don't override what someone already told us
	if (m_nDisconnectCode)
		return;

	m_nDisconnectCode = nCode;

	if( pMsg )
		m_sDisconnectMsg = pMsg;
	else
		m_sDisconnectMsg.Empty( );
}

// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::ClearDisconnectCode
//
//	PURPOSE:	Clears the disconnection code and message
//
// --------------------------------------------------------------------------- //

void ClientMultiplayerMgr::ClearDisconnectCode()
{
	m_nDisconnectCode = 0;
	m_sDisconnectMsg.Empty( );
	m_StartGameRequest.m_Type = GAMEMODE_NONE;
}

// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::CreateServerDir
//
//	PURPOSE:	Creates the client's serverdir for joining a remote game.
//
// --------------------------------------------------------------------------- //

/*
IServerDirectory* ClientMultiplayerMgr::CreateServerDir( )
{
	// Make sure we don't already have one.
	DeleteServerDir( );

	// Get the resource module so we can give it to the serverdir for
	// error messages.
	void* pModule = NULL;
	g_pLTClient->GetEngineHook("cres_hinstance",&pModule);
	HMODULE hModule = (HINSTANCE)pModule;

	m_pServerDir = Factory_Create_IServerDirectory_Titan( true, *g_pLTClient, hModule );
	if( !m_pServerDir )
		return NULL;

	// Set the game's name
	m_pServerDir->SetGameName(g_pVersionMgr->GetNetGameName());
	// Set the version
	m_pServerDir->SetVersion(g_pVersionMgr->GetNetVersion());
	m_pServerDir->SetRegion(g_pVersionMgr->GetNetRegion());
	// Set up the packet header
	CAutoMessage cMsg;
	cMsg.Writeuint8(11); // CMSG_MESSAGE
	cMsg.Writeuint8(MID_MULTIPLAYER_SERVERDIR);
	m_pServerDir->SetNetHeader(*cMsg.Read());

	return m_pServerDir;
	return NULL;
}
*/

// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::DeleteServerDir
//
//	PURPOSE:	Remove the server dir.
//
// --------------------------------------------------------------------------- //

/*
void ClientMultiplayerMgr::DeleteServerDir( )
{ 
	if( m_pServerDir )
	{
		// No leaking, please...
		delete m_pServerDir; 
		m_pServerDir = NULL; 
	}
}
*/

// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::SetCurMessageSource
//
//	PURPOSE:	Set the source address of the message which is currently being processed
//
// --------------------------------------------------------------------------- //

void ClientMultiplayerMgr::SetCurMessageSource(const uint8 aAddr[4], uint16 nPort)
{
	m_aCurMessageSourceAddr[0] = aAddr[0];
	m_aCurMessageSourceAddr[1] = aAddr[1];
	m_aCurMessageSourceAddr[2] = aAddr[2];
	m_aCurMessageSourceAddr[3] = aAddr[3];
	m_nCurMessageSourcePort = nPort;
}

// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::GetCurMessageSource
//
//	PURPOSE:	Get the source address of the message which is currently being processed
//
// --------------------------------------------------------------------------- //

void ClientMultiplayerMgr::GetCurMessageSource(uint8 aAddr[4], uint16 *pPort)
{
	aAddr[0] = m_aCurMessageSourceAddr[0];
	aAddr[1] = m_aCurMessageSourceAddr[1];
	aAddr[2] = m_aCurMessageSourceAddr[2];
	aAddr[3] = m_aCurMessageSourceAddr[3];
	*pPort = m_nCurMessageSourcePort;
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::OnMessage()
//
//	PURPOSE:	Handle client messages
//
// ----------------------------------------------------------------------- //

bool ClientMultiplayerMgr::OnMessage(uint8 messageID, ILTMessage_Read *pMsg)
{
	// Check inputs.
	if( !pMsg )
	{
		ASSERT( !"ClientMultiplayerMgr::OnMessage: Invalid msg." );
		return false;
	}

	switch(messageID)
	{
		case MID_HANDSHAKE:
			return HandleMsgHandshake( *pMsg );
			break;
		case MID_PLAYER_SINGLEPLAYER_INIT:	
			return HandleMsgPlayerSingleplayerInit	( *pMsg);	
			break;
		case MID_PLAYER_MULTIPLAYER_INIT:	
			return HandleMsgPlayerMultiplayerInit( *pMsg );
			break;
		case MID_MULTIPLAYER_DATA:
			return HandleMsgMultiplayerData( *pMsg );
			break;
/*		case MID_MULTIPLAYER_SERVERDIR:
			if (GetServerDir())
			{
				char aAddrBuffer[16];
				sprintf(aAddrBuffer, "%d.%d.%d.%d", 
					(uint32)m_aCurMessageSourceAddr[0], 
					(uint32)m_aCurMessageSourceAddr[1], 
					(uint32)m_aCurMessageSourceAddr[2], 
					(uint32)m_aCurMessageSourceAddr[3]);
				return GetServerDir()->HandleNetMessage(*CLTMsgRef_Read(pMsg->SubMsg(pMsg->Tell())), aAddrBuffer, m_nCurMessageSourcePort);
			}
			else
				return false;
*/
			break;
		default:
			break;
		
	}

	return false;
}


bool ClientMultiplayerMgr::HandleMsgHandshake( ILTMessage_Read & msg )
{
	int nHandshakeSub = (int)msg.Readuint8();
	switch (nHandshakeSub)
	{
		case MID_HANDSHAKE_HELLO :
		{
			int nHandshakeVer = (int)msg.Readuint16();
			if (nHandshakeVer != GAME_HANDSHAKE_VER)
			{
				// Disconnect
				m_bForceDisconnect = true;
				SetDisconnectCode(eDisconnect_NotSameGUID,NULL);

				return true;
			}

			// Send back a hello response
			CAutoMessage cResponse;
			cResponse.Writeuint8(MID_HANDSHAKE);
			cResponse.Writeuint8(MID_HANDSHAKE_HELLO);
		    cResponse.Writeuint16(GAME_HANDSHAKE_VER);
			// Send them our secret key
			cResponse.Writeuint32( GAME_HANDSHAKE_PASSWORD );
			g_pLTClient->SendToServer(cResponse.Read(), MESSAGE_GUARANTEED);
		}
		break;
		case MID_HANDSHAKE_PASSWORD:
		{
			// Read in their key
			m_nServerKey = msg.Readuint32();
			bool   bNeedPassword = msg.Readbool();

			// CJ 1.1 changed the handshake (verified against retail CSHELL.DLL / OBJECT.LTO):
			// the server sends key = h(c) ^ X ^ serverSecret, where X is the value we sent in
			// HELLO and c is the first char of our client name. The XOR mask it checks the CRCs
			// with is h(c) ^ serverSecret == key ^ X. The password field is mask ^ first 4
			// bytes of the player GUID.
			uint32 nPassword = GAME_HANDSHAKE_PASSWORD;
			uint32 nXORMask = GAME_HANDSHAKE_MASK;
			{
				char szGuid[256];
				memset( szGuid, 0, sizeof( szGuid ));
				CUserProfile* pHsProfile = g_pProfileMgr ? g_pProfileMgr->GetCurrentProfile() : NULL;
				if( pHsProfile )
					strncpy( szGuid, pHsProfile->m_sPlayerGuid.c_str( ), 254 );
				uint32 nFirst4 = 0;
				memcpy( &nFirst4, szGuid, 4 );
				nXORMask = m_nServerKey ^ ( uint32 )GAME_HANDSHAKE_PASSWORD;
				nPassword = nXORMask ^ nFirst4;
			}

			// Game password.
			uint32 nHashedPassword = 0;
			if (bNeedPassword)
			{
				nHashedPassword = m_nClientPass;
			}

			// Get the weapons file CRC
			uint32 nWeaponCRC = g_pWeaponMgr->GetFileCRC();
			// Mask that up too
			nWeaponCRC ^= nXORMask;

			// CRC the modelbutes.txt
			uint32 nModelButesCRC = g_pModelButeMgr->GetFileCRC( );
			uint32 nModelButesMaskedCRC = nModelButesCRC ^ nXORMask;

			// CRC the surface.txt
			static uint32 nSurfaceCRC = CRC32::CalcRezFileCRC( g_pSurfaceMgr->GetAttributeFile( ));
			uint32 nSurfaceMaskedCRC = nSurfaceCRC ^ nXORMask;

			// CRC the damagefx.txt
			static uint32 nDamageFxCRC = CRC32::CalcRezFileCRC( "attributes\\damagefx.txt" );
			uint32 nDamageFxMaskedCRC = nDamageFxCRC ^ nXORMask;

			// CRC the autoexec.cfg
			static uint32 nAutoexecCRC = CRC32::CalcFileCRC( "autoexec.cfg" );
			uint32 nAutoexecMaskedCRC = nAutoexecCRC ^ nXORMask;

			// Get the client shell file CRC
			char aClientShellName[MAX_PATH + 1];
			// Just in case getting the file name fails
			aClientShellName[0] = 0; 
			// Get the client shell handle from the engine
			HMODULE hClientShell;
			g_pLTClient->GetEngineHook("cshell_hinstance", (void**)&hClientShell);
			DWORD nResult = GetModuleFileName(hClientShell, aClientShellName, sizeof(aClientShellName));
			// Pass the real name of the file, since we'll be working with a possible temporary named
			// cshell.dll.
			uint32 nClientCRC = CRC32::CalcFileCRC(aClientShellName, "cshell.dll" );
			
			// Mask that up too
			nClientCRC ^= nXORMask;


			// Check if we need to send the cdkey.
			bool bNeedsCDKey = msg.Readbool( );
			char szChallenge[256] = "";
			char szChallengeResponse[256] = "";
			if( bNeedsCDKey )
			{
				msg.ReadString( szChallenge, ARRAY_LEN( szChallenge ));
				char szCDKey[256];
				g_pVersionMgr->GetCDKey( szCDKey, ARRAY_LEN( szCDKey ));
				uint32 nChallengeResponseLen = ARRAY_LEN( szChallengeResponse );

				// Make sure we have a serverbrowser object.
				if( !m_pRetailGameSpyBrowser )
				{
					CreateServerBrowsers( );
				}

				m_pRetailGameSpyBrowser->CDKeyComputeChallengeResponse( szCDKey, szChallenge, szChallengeResponse, nChallengeResponseLen );
			}

			// Send it back their direction
			CAutoMessage cResponse;
			cResponse.Writeuint8(MID_HANDSHAKE);
			cResponse.Writeuint8(MID_HANDSHAKE_LETMEIN);
			cResponse.Writeuint32(nPassword);
			cResponse.Writeuint32(nWeaponCRC);
			cResponse.Writeuint32(nClientCRC);
			cResponse.Writeuint32(nModelButesMaskedCRC);
			cResponse.Writeuint32(nSurfaceMaskedCRC);
			cResponse.Writeuint32(nDamageFxMaskedCRC);
			cResponse.Writeuint32(nAutoexecMaskedCRC);
			cResponse.Writeuint32(nHashedPassword);
			if( bNeedsCDKey )
			{
				cResponse.WriteString( szChallenge );
				cResponse.WriteString( szChallengeResponse );
			}
			g_pLTClient->SendToServer(cResponse.Read(), MESSAGE_GUARANTEED);
		}
		break;
		case MID_HANDSHAKE_DONE:
		{
			// This just means the server validated us...
		}
		break;
		case MID_HANDSHAKE_INVALIDASSETS:
		{
			// Oops... wrong password, disconnect
			m_bForceDisconnect = true;
			SetDisconnectCode(eDisconnect_InvalidAssets, NULL);
		}
		break;
		case MID_HANDSHAKE_WRONGPASS:
		{
			// Oops... wrong password, disconnect
			m_bForceDisconnect = true;
			SetDisconnectCode(eDisconnect_WrongPassword, NULL);
		}
		break;
		case MID_HANDSHAKE_BADCDKEY:
		{
			// Bad cdkey.
			m_bForceDisconnect = true;
			SetDisconnectCode(eDisconnect_BadCdKey, NULL);
		}
		break;
		case MID_HANDSHAKE_BANNED:
		{
			// I'm a baaaaaad boy....
			m_bForceDisconnect = true;
			SetDisconnectCode(eDisconnect_Banned, NULL);
		}
		break;
		default :
		{
			// Disconnect
			m_bForceDisconnect = true;
			SetDisconnectCode(eDisconnect_NotSameGUID, NULL);
		}
		break;
	}

	return true;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::HandleMsgPlayerSingleplayerInit()
//
//	PURPOSE:	
//
// ----------------------------------------------------------------------- //

bool ClientMultiplayerMgr::HandleMsgPlayerSingleplayerInit (ILTMessage_Read& msg)
{
	return InitSinglePlayer();
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::HandleMsgPlayerMultiplayerInit()
//
//	PURPOSE:	
//
// ----------------------------------------------------------------------- //

bool ClientMultiplayerMgr::HandleMsgPlayerMultiplayerInit(ILTMessage_Read& msg)
{
	return InitMultiPlayer();
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::HandleMsgMultiplayerData()
//
//	PURPOSE:	Read multiplayer data sent from server.
//
// ----------------------------------------------------------------------- //

bool ClientMultiplayerMgr::HandleMsgMultiplayerData( ILTMessage_Read& msg )
{
    GameType eGameType = ( GameType )msg.Readuint8();
	g_pGameClientShell->SetGameType( eGameType );

	// Check if some joker set us to single player.
	if( eGameType == eGameTypeSingle )
	{
		ASSERT( !"ClientMultiplayerMgr::HandleMsgMultiplayerData: Invalid game type." );
		return false;
	}

	msg.ReadString( m_sServerAddress.GetBuffer( 256 ), 256 );
	m_sServerAddress.ReleaseBuffer( );
	uint32 tmp = msg.Readuint32();
	m_nServerPort = (int)tmp;

	return true;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::OnEvent()
//
//	PURPOSE:	Called for asynchronous errors that cause the server
//				to shut down
//
// ----------------------------------------------------------------------- //

void ClientMultiplayerMgr::OnEvent(uint32 dwEventID, uint32 dwParam)
{
	if( !IsMultiplayerGame( ))
		return;

	switch(dwEventID)
	{
		// Client disconnected from server.  dwParam will
		// be a error flag found in de_codes.h.
		case LTEVENT_DISCONNECT :
		{
			m_sServerAddress.Empty( );
			m_nServerPort = -1;
			m_sServerName.Empty( );
			m_nServerKey = 0;
		} 
		break;
	}
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::Update()
//
//	PURPOSE:	Frame update.
//
// ----------------------------------------------------------------------- //

void ClientMultiplayerMgr::Update( )
{
	// This will happen when something wanted to disconnect, but wasn't
	// in a valid location to do so.  (e.g. when processing packets..)
	if (m_bForceDisconnect)
	{
		g_pLTClient->Disconnect();
		m_bForceDisconnect = false;
		return;
	}

	UpdateConnectionState( );
}

// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::SetService
//
//	PURPOSE:	Selects the connection service for hosting/joining internet games.
//
// --------------------------------------------------------------------------- //

bool ClientMultiplayerMgr::SetService( )
{
	NetService *pCur, *pListHead;
	HNETSERVICE hNetService;

	pCur      = NULL;
	pListHead = NULL;
	hNetService = NULL;

    if( g_pLTClient->GetServiceList( pListHead ) != LT_OK || !pListHead )
        return false;

	// Find the service specified.
	pCur = pListHead;
	while( pCur )
	{
		if( pCur->m_dwFlags & NETSERVICE_TCPIP )
		{
			hNetService = pCur->m_handle;
			break;
		}

		pCur = pCur->m_pNext;
	}

	// Free the service list.
    g_pLTClient->FreeServiceList( pListHead );

	// Check if tcp not found.
	if( !hNetService )
        return false;

	// Select it.
    if( g_pLTClient->SelectService( hNetService ) != LT_OK )
        return false;

    return true;
}


// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::StartClient
//
//	PURPOSE:	Start a client of a remote server.
//
// --------------------------------------------------------------------------- //

bool ClientMultiplayerMgr::StartClient( )
{
	// Start off disconnected.
	m_eConnectionState = eConnectionState_Disconnected;

	// Initialize the networking.
    m_nLastConnectionResult = g_pLTClient->InitNetworking(NULL, 0);
	if (m_nLastConnectionResult != LT_OK)
	{
        return false;
	}

	// Initialize our protocol.
	if (!SetService())
        return false;

	// Hook up the netgame and clientinfo.
	m_StartGameRequest.m_pClientData = &m_NetClientData;
	m_StartGameRequest.m_ClientDataLen = sizeof( m_NetClientData );

	// If we don't need natneg, then go right to connecting.
	if( !m_bDoNatNegotiations )
	{
		m_eConnectionState = eConnectionState_Connecting;
		return true;
	}

	// Convert the address string into ip and port.
	char szIP[256];
	uint16 nPort;
	char* pszPortDelim = strchr( m_StartGameRequest.m_TCPAddress, ':' );
	if( !pszPortDelim || !pszPortDelim[1] )
		return false;
	uint32 nIPLen = pszPortDelim - m_StartGameRequest.m_TCPAddress;
	strncpy( szIP, m_StartGameRequest.m_TCPAddress, nIPLen );
	szIP[nIPLen] = 0;
	nPort = atoi( pszPortDelim + 1 );

	// Start a ping request.  This just gets the networking rolling.
	SOCKET hSocket;
	// OpenSocket is not exposed by the CJ 1.0 SDK headers: NAT negotiation disabled.
	hSocket = INVALID_SOCKET;
	if( true )
		return false;

	// Make sure we have a serverbrowser object.
	if( !m_pRetailGameSpyBrowser )
	{
		CreateServerBrowsers( );
	}

	if( m_pRetailGameSpyBrowser && hSocket )
	{
		// Start natneg process.
		if( m_pRetailGameSpyBrowser->RequestNatNegotiation( hSocket, szIP, nPort ))
		{
			m_StartGameRequest.m_nSocket = hSocket;
			m_eConnectionState = eConnectionState_NatNeg;
			return true;
		}
	}

	// Couldn't get natneg going, just do regular connecting.
	m_eConnectionState = eConnectionState_Connecting;
	return true;
}


// ---------------------------------------------------------------------------
// Contract J.A.C.K. port
//
// The retail server (Object.lto) was built with Visual C++ 7.1 and reads the
// client's ServerGameOptions directly out of our memory.  Visual Studio 2022's
// std::string / std::set have a different layout, so handing the server our own
// object makes it crash.  Instead we build a byte-for-byte image of what the
// VC7.1 build of ServerGameOptions looks like (layout taken from the retail
// CSHELL.DLL) and give the server that.
// ---------------------------------------------------------------------------
namespace CJ71
{
	static const int kImageSize = 0x428;
	static const int kBaseSize = 0x94;
	static void* s_aDummyVtbl[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

	static void PutU32( char* b, int o, uint32 v ) { memcpy( b + o, &v, 4 ); }
	static void PutU16( char* b, int o, uint16 v ) { memcpy( b + o, &v, 2 ); }
	static void PutU8( char* b, int o, uint8 v ) { b[o] = (char)v; }
	static void PutF32( char* b, int o, float v ) { memcpy( b + o, &v, 4 ); }

	// VC7.1 std::string: 28 bytes.  [0] allocator, [4] buf/ptr union, [20] size, [24] reserve.
	static void PutStr( char* b, int o, const std::string& s )
	{
		char* p = b + o;
		memset( p, 0, 28 );
		size_t n = s.size();
		if( n < 16 )
		{
			memcpy( p + 4, s.c_str( ), n );
			PutU32( p, 20, (uint32)n );
			PutU32( p, 24, 15 );
		}
		else
		{
			char* h = (char*)malloc( n + 1 );
			memcpy( h, s.c_str( ), n + 1 );
			memcpy( p + 4, &h, 4 );
			PutU32( p, 20, (uint32)n );
			PutU32( p, 24, (uint32)n );
		}
	}

	// VC7.1 tree node for set<string>: _Left, _Parent, _Right, string(28), _Color, _Isnil.
	static char* NewNode( char* head, const std::string* v )
	{
		char* n = (char*)calloc( 1, 44 );
		if( v )
			PutStr( n, 12, *v );
		memcpy( n + 0, &head, 4 );
		memcpy( n + 4, &head, 4 );
		memcpy( n + 8, &head, 4 );
		n[40] = 1;	// black
		n[41] = 0;
		return n;
	}

	static char* BuildTree( char* head, const std::vector< const std::string* >& v, int lo, int hi, char* parent )
	{
		if( lo >= hi )
			return head;
		int mid = ( lo + hi ) / 2;
		char* n = NewNode( head, v[mid] );
		memcpy( n + 4, &parent, 4 );
		char* l = BuildTree( head, v, lo, mid, n );
		char* r = BuildTree( head, v, mid + 1, hi, n );
		memcpy( n + 0, &l, 4 );
		memcpy( n + 8, &r, 4 );
		return n;
	}

	// VC7.1 std::set<string>: 12 bytes.  [4] head node, [8] size.
	static void PutSet( char* b, int o, const StringSet& s )
	{
		char* p = b + o;
		memset( p, 0, 12 );
		char* head = NewNode( NULL, NULL );
		head[41] = 1;	// _Isnil
		memcpy( head + 0, &head, 4 );
		memcpy( head + 4, &head, 4 );
		memcpy( head + 8, &head, 4 );

		std::vector< const std::string* > v;
		for( StringSet::const_iterator it = s.begin( ); it != s.end( ); ++it )
			v.push_back( &(*it) );

		if( !v.empty( ) )
		{
			char* root = BuildTree( head, v, 0, (int)v.size( ), head );
			char* lm = root; while( 1 ) { char* l; memcpy( &l, lm + 0, 4 ); if( l == head ) break; lm = l; }
			char* rm = root; while( 1 ) { char* r; memcpy( &r, rm + 8, 4 ); if( r == head ) break; rm = r; }
			memcpy( head + 4, &root, 4 );
			memcpy( head + 0, &lm, 4 );
			memcpy( head + 8, &rm, 4 );
		}
		memcpy( p + 4, &head, 4 );
		PutU32( p, 8, (uint32)v.size( ) );
	}

	static void PutBase( char* b, int o, BaseGameOptions& g )
	{
		char* p = b + o;
		memset( p, 0, kBaseSize );
		memcpy( p, &s_aDummyVtbl, 0 );
		void* vt = (void*)s_aDummyVtbl;
		memcpy( p, &vt, 4 );
		PutStr( p, 0x04, g.m_sSessionName );
		PutStr( p, 0x20, g.m_sCampaignName );
		PutU8( p, 0x3c, g.m_nMaxPlayers );
		PutU8( p, 0x3d, g.m_bFriendlyFire ? 1 : 0 );
		PutU8( p, 0x3e, g.m_bWeaponsStay ? 1 : 0 );
		PutU8( p, 0x3f, g.m_bUseSkills ? 1 : 0 );
		PutU8( p, 0x40, g.m_nDifficulty );
		PutF32( p, 0x44, g.m_fPlayerDiffFactor );
		PutU8( p, 0x48, g.m_nRunSpeed );
		PutU8( p, 0x49, g.m_nScoreLimit );
		PutU8( p, 0x4a, g.m_nTimeLimit );
		PutU8( p, 0x4b, g.m_nRounds );
		PutU8( p, 0x4c, g.m_nFragScore );
		PutU8( p, 0x4d, g.m_nRevivingScore );
		PutU8( p, 0x4e, g.m_nNumTeams );
		for( int i = 0; i < MAX_TEAMS; i++ )
		{
			PutU32( p, 0x50 + i * 4, g.m_nTeamModel[i] );
			PutStr( p, 0x58 + i * 28, g.m_sTeamName[i] );
		}
		PutU8( p, 0x90, g.m_nDeviceCompletedScore );
		PutU8( p, 0x91, g.m_nLightPiecePlacedScore );
		PutU8( p, 0x92, g.m_nHeavyPiecePlacedScore );
		PutU8( p, 0x93, g.m_nPieceRemovedScore );
	}

	// Builds the VC7.1-layout image.  The image is kept alive for the process lifetime.
	static char* BuildImage( ServerGameOptions& o )
	{
		static char* s_pImage = NULL;
		if( !s_pImage )
			s_pImage = (char*)calloc( 1, kImageSize );
		char* b = s_pImage;
		memset( b, 0, kImageSize );

		PutU32( b, 0x00, (uint32)o.m_eGameType );
		PutU8( b, 0x04, o.m_bUsePassword ? 1 : 0 );
		PutStr( b, 0x08, o.m_sPassword );
		PutStr( b, 0x24, o.m_sScmdPassword );
		PutU8( b, 0x40, o.m_bAllowScmdCommands ? 1 : 0 );
		PutU16( b, 0x42, o.m_nPort );
		PutU8( b, 0x44, o.m_nBandwidthServer );
		PutU16( b, 0x46, o.m_nBandwidthServerCustom );
		PutU8( b, 0x48, o.m_bLANOnly ? 1 : 0 );
		PutU8( b, 0x49, o.m_bDedicated ? 1 : 0 );
		PutSet( b, 0x4c, o.m_setRestrictedWeapons );
		PutSet( b, 0x58, o.m_setRestrictedAmmo );
		PutSet( b, 0x64, o.m_setRestrictedGear );
		PutStr( b, 0x70, o.m_sModName );
		PutStr( b, 0x8c, o.m_sProfileName );
		PutU8( b, 0xa8, o.m_bPreCacheAssets ? 1 : 0 );
		PutU8( b, 0xa9, o.m_bPerformanceTest ? 1 : 0 );
		PutU32( b, 0xac, (uint32)o.m_eServerStartResult );

		PutBase( b, 0x0b0, o.GetSingleplayer( ));
		PutBase( b, 0x144, o.GetCoop( ));
		PutBase( b, 0x1d8, o.GetDeathmatch( ));
		PutBase( b, 0x26c, o.GetTeamDeathmatch( ));
		PutBase( b, 0x300, o.GetDemolition( ));
		PutBase( b, 0x394, o.GetDoomsday( ));
		return b;
	}

	static void ReadBack( char* pImage, ServerGameOptions& o )
	{
		uint32 r = 0;
		memcpy( &r, pImage + 0xac, 4 );
		o.m_eServerStartResult = (EServerStartResult)r;
	}
}

// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::StartServerHost
//
//	PURPOSE:	Start a hosted game.
//
// --------------------------------------------------------------------------- //

bool ClientMultiplayerMgr::StartServerAsHost( )
{
	// If they want a dedicated server, then launch the serverapp.
	if( m_ServerGameOptions.m_bDedicated )
	{
		if( !g_pGameClientShell->LauncherServerApp( m_ServerGameOptions.m_sProfileName.c_str( )))
			return false;

		return true;
	}

	// Check if we're already connected to a server.
  	if( g_pLTClient->IsConnected( ))
	{
  		// Check if we are already hosting mp.
  		if( IsMultiplayerGame( ) && m_StartGameRequest.m_Type == STARTGAME_HOST )
  		{
  			// Don't need to restart a server.
  			return true;
		}
	}

	// Initialize the networking.  Always start a new server with hosted games.
    m_nLastConnectionResult = g_pLTClient->InitNetworking(NULL, 0);
	if (m_nLastConnectionResult != LT_OK)
	{
        return false;
	}

	// Initialize our protocol.
	if (!SetService())
        return false;

	// Make sure we throw away the serverbrowser objects.
	TermBrowsers( );

	// Hook up the netgame and clientinfo.
	char* pServerGameOptions = CJ71::BuildImage( m_ServerGameOptions );
	m_StartGameRequest.m_pGameInfo = &pServerGameOptions;
	m_StartGameRequest.m_GameInfoLen = sizeof( pServerGameOptions );
	m_StartGameRequest.m_pClientData = &m_NetClientData;
	m_StartGameRequest.m_ClientDataLen = sizeof( m_NetClientData );

	// Go right to being connected.
	m_eConnectionState = eConnectionState_Connected;

	// Start the server.
	m_nLastConnectionResult = g_pLTClient->StartGame( const_cast< StartGameRequest * >( &m_StartGameRequest ));
	CJ71::ReadBack( pServerGameOptions, m_ServerGameOptions );

	return ( m_nLastConnectionResult == LT_OK );
}

// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::StartServerSinglePlayer
//
//	PURPOSE:	Starts the single player server.
//
// --------------------------------------------------------------------------- //

bool ClientMultiplayerMgr::StartServerAsSinglePlayer( )
{
	// Check if we're already connected to a server.
  	if( g_pLTClient->IsConnected( ))
	{
  		// Check if we are already running sp server.
  		if( !IsMultiplayerGame( ) && m_StartGameRequest.m_Type == STARTGAME_NORMAL )
  		{
  			// Don't need to restart a server.
  			return true;
		}
	}

	// Make sure the profile is set.
	m_ServerGameOptions.m_eGameType = g_pGameClientShell->GetGameType( );
	char* pServerGameOptions = CJ71::BuildImage( m_ServerGameOptions );
	m_StartGameRequest.m_pGameInfo = &pServerGameOptions;
	m_StartGameRequest.m_GameInfoLen = sizeof( pServerGameOptions );


	// Make sure we throw away the serverbrowser objects.
	TermBrowsers( );

	// Start with clean slate
	m_StartGameRequest.m_Type = STARTGAME_NORMAL;

	// Go right to being connected.
	m_eConnectionState = eConnectionState_Connected;

    m_nLastConnectionResult = g_pLTClient->StartGame(&m_StartGameRequest);
	CJ71::ReadBack( pServerGameOptions, m_ServerGameOptions );

	return ( m_nLastConnectionResult == LT_OK );
}

// --------------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::StartClientServer
//
//	PURPOSE:	Starts a client/server based on previously set startgamerequest.
//
// --------------------------------------------------------------------------- //

bool ClientMultiplayerMgr::StartClientServer( )
{
	//clear out old session specific data before starting new session
	m_nTeam = INVALID_TEAM;
	m_bHasSelectedTeam = false;
	m_ServerGameOptions.m_eServerStartResult = eServerStartResult_None;

	switch( m_StartGameRequest.m_Type )
	{
		case STARTGAME_NORMAL:
			return StartServerAsSinglePlayer( );
			break;
		case STARTGAME_HOST:
			return StartServerAsHost( );
			break;
		case STARTGAME_CLIENTTCP:
			return StartClient( );
			break;
		default:
			ASSERT( !"ClientMultiplayerMgr::StartClientServer: Invalid gamerequest type." );
			return false;
			break;
	}
}




// ----------------------------------------------------------------------- //
//
//	ROUTINE:	ClientMultiplayerMgr::SelectTeam()
//
//	PURPOSE:	choose a team.
//
// ----------------------------------------------------------------------- //

void ClientMultiplayerMgr::SelectTeam(uint8 nTeam, bool bPlayerSelected )
{
	if (bPlayerSelected)
		m_bHasSelectedTeam = true;
	m_nTeam = nTeam;
}

// --------------------------------------------------------------------------- //
//
//  ROUTINE:    ClientMultiplayerMgr::UpdateNetClientData
//
//  PURPOSE:    Updates the NetClientData to reflect current settings.
//
// --------------------------------------------------------------------------- //

bool ClientMultiplayerMgr::UpdateNetClientData( )
{
	// Setup our client...
	CUserProfile *pProfile = g_pProfileMgr->GetCurrentProfile();
	if( !pProfile )
	{
		ASSERT( !"ClientMultiplayerMgr::UpdateNetClientData: Invalid profile." );
		return false;
	}

	// Check if the user overrode the player name on the command line.
	char szPlayerNameOverride[MAX_PLAYER_NAME] = "";
	LTStrCpy( szPlayerNameOverride, FixPlayerName(GetConsoleTempString( "playername", "" )), ARRAY_LEN( szPlayerNameOverride ));

	if( szPlayerNameOverride[0] )
	{
		pProfile->m_sPlayerName = szPlayerNameOverride;
	}

	LTStrCpy(m_NetClientData.m_szName,pProfile->m_sPlayerName.c_str(), ARRAY_LEN( m_NetClientData.m_szName ));

	// Setup the name guid.
	SAFE_STRCPY(m_NetClientData.m_szPlayerGuid,pProfile->m_sPlayerGuid.c_str( ));

	// Setup the model.
	switch (g_pGameClientShell->GetGameType())
	{
		case eGameTypeCooperative:
			m_NetClientData.m_ePlayerModelId = g_pModelButeMgr->GetCPModel( pProfile->m_nCPPlayerModel );
			break;
		case eGameTypeDeathmatch:
			m_NetClientData.m_ePlayerModelId = g_pModelButeMgr->GetDMModel( pProfile->m_nDMPlayerModel );
			break;
	};

	return true;	
}

// ----------------------------------------------------------------------- //
// Function name   : SetupGameSpyBrowser
// Description     : Sets the gamespybrowser object up with property keys.
// Return type     : static bool - true on success.
// Argument        : IGameSpyBrowser& gameSpyBrowser - browser to setup.
// ----------------------------------------------------------------------- //
static bool SetupGameSpyBrowser( IGameSpyBrowser& gameSpyBrowser )
{
	// Register the keys used.
	gameSpyBrowser.RegisterKey( "hostname" );
	gameSpyBrowser.RegisterKey( "mapname" );
	gameSpyBrowser.RegisterKey( "numplayers" );
	gameSpyBrowser.RegisterKey( "maxplayers" );
	gameSpyBrowser.RegisterKey( "gametype" );
	gameSpyBrowser.RegisterKey( "gamemode" );
	gameSpyBrowser.RegisterKey( "password" );
	gameSpyBrowser.RegisterKey( "gamever" );
	gameSpyBrowser.RegisterKey( "fraglimit" );
	gameSpyBrowser.RegisterKey( "timelimit" );
	gameSpyBrowser.RegisterKey( "player_" );
	gameSpyBrowser.RegisterKey( "frags_" );
	gameSpyBrowser.RegisterKey( "ping_" );
	gameSpyBrowser.RegisterKey( "modname" );
	gameSpyBrowser.RegisterKey( "options" );

	// Use these keys for the summary.
	gameSpyBrowser.AddSummaryKey( "hostname" );
	gameSpyBrowser.AddSummaryKey( "numplayers" );
	gameSpyBrowser.AddSummaryKey( "maxplayers" );
	gameSpyBrowser.AddSummaryKey( "gametype" );
	gameSpyBrowser.AddSummaryKey( "gamever" );
	gameSpyBrowser.AddSummaryKey( "modname" );
	gameSpyBrowser.AddSummaryKey( "password" );

	return true;
}

// ----------------------------------------------------------------------- //
// Function name   : ClientMultiplayerMgr::CreateServerBrowsers
// Description     : Creates the serverbrowser objects needed for joining
//						public/lan retail/demo servers.
// Return type     : static bool - true on success.
// ----------------------------------------------------------------------- //
bool ClientMultiplayerMgr::CreateServerBrowsers( )
{
	if( !m_pRetailGameSpyBrowser )
	{
		// Create the retail browser.
		IGameSpyBrowser::StartupInfo startupInfo;
		startupInfo.m_eGameSKU = eGameSKU_ContractJack_Retail;
		m_pRetailGameSpyBrowser = IGameSpyBrowser::Create( startupInfo );
		SetupGameSpyBrowser( *m_pRetailGameSpyBrowser );
	}

	if( !m_pDemoGameSpyBrowser )
	{
		// Create the mpdemo browser.
		IGameSpyBrowser::StartupInfo startupInfo;
#if defined( _PRDEMO )
		startupInfo.m_eGameSKU = eGameSKU_ContractJack_PRDemo;
#else // defined( _PRDEMO )
		startupInfo.m_eGameSKU = eGameSKU_ContractJack_MPDemo;
#endif // defined( _PRDEMO )
		m_pDemoGameSpyBrowser = IGameSpyBrowser::Create( startupInfo );
		SetupGameSpyBrowser( *m_pDemoGameSpyBrowser );
	}

	return true;
}

// ----------------------------------------------------------------------- //
// Function name   : ClientMultiplayerMgr::TermBrowsers
// Description     : Deletes the server browser objects.  Good idea
//						to delete them when you are done with them,
//						since they can store lots of data in the server lists.
// ----------------------------------------------------------------------- //
void ClientMultiplayerMgr::TermBrowsers( )
{
	// Delete the browsers now that we're joining.
	if( m_pRetailGameSpyBrowser )
	{
		IGameSpyBrowser::Delete( m_pRetailGameSpyBrowser );
		m_pRetailGameSpyBrowser = NULL;
	}
	if( m_pDemoGameSpyBrowser )
	{
		IGameSpyBrowser::Delete( m_pDemoGameSpyBrowser );
		m_pDemoGameSpyBrowser = NULL;
	}
}

// ----------------------------------------------------------------------- //
// Function name   : ClientMultiplayerMgr::UpdateConnectionState
// Description     : Updates the statemachine.
// ----------------------------------------------------------------------- //
void ClientMultiplayerMgr::UpdateConnectionState( )
{
	switch( m_eConnectionState )
	{
		case eConnectionState_Disconnected:
			break;
		case eConnectionState_NatNeg:
			{
				UpdateState_NatNeg( );
			}
			break;
		case eConnectionState_SettleComm:
			{
				UpdateState_SettleComm( );
			}
			break;
		case eConnectionState_Connecting:
			{
				UpdateState_Connecting( );
			}
			break;
		case eConnectionState_Connected:
			break;
		case eConnectionState_Failure:
			{
				g_pInterfaceMgr->LoadFailed( );
				m_eConnectionState = eConnectionState_Disconnected;
			}
			break;
	}
}

// ----------------------------------------------------------------------- //
// Function name   : ClientMultiplayerMgr::UpdateState_NatNeg
// Description     : Updates the natneg state.
// ----------------------------------------------------------------------- //
void ClientMultiplayerMgr::UpdateState_NatNeg( )
{
	switch( m_pRetailGameSpyBrowser->GetBrowserStatus( ))
	{
		case IGameSpyBrowser::eBrowserStatus_Processing:
			break;
		case IGameSpyBrowser::eBrowserStatus_Error:
		case IGameSpyBrowser::eBrowserStatus_Idle:
			// Drop the socket, we don't need it any more.
			m_StartGameRequest.m_nSocket = StartGameRequest::kInvalidSocket;
			m_eConnectionState = eConnectionState_Connecting;
			break;
		case IGameSpyBrowser::eBrowserStatus_Complete:
			sockaddr_in sockAddr;
			m_pRetailGameSpyBrowser->GetNatNegotiationResult( ( sockaddr* )( &sockAddr ));

			// Change our address to the one found.
			sprintf( m_StartGameRequest.m_TCPAddress, ADDR_PRINTF, EXPAND_ADDR( sockAddr ));

			m_SettleCommTimer.Start( 2.0f );
			m_eConnectionState = eConnectionState_SettleComm;
			break;
	}
}

// ----------------------------------------------------------------------- //
// Function name   : ClientMultiplayerMgr::UpdateState_SettleComm
// Description     : Lets comm settle for a few seconds.
// ----------------------------------------------------------------------- //
void ClientMultiplayerMgr::UpdateState_SettleComm( )
{
	if( m_SettleCommTimer.Stopped( ))
	{
		m_eConnectionState = eConnectionState_Connecting;
	}
}

// ----------------------------------------------------------------------- //
// Function name   : ClientMultiplayerMgr::UpdateState_Connecting
// Description     : Updates the connecting state.
// ----------------------------------------------------------------------- //
void ClientMultiplayerMgr::UpdateState_Connecting( )
{
	// We will try to connect to the server using several methods.  First, we'll use
	// the default address that we have determined through gamespy.  This may or may
	// not have been setup through natneg.  Then we'll try to connect to the private address.
	// If that doesn't work, we'll try the public address.  If none of those work, we'll 
	// give up.
	enum EConnectAddress
	{
		eConnectAddress_Default,
		eConnectAddress_Private,
		eConnectAddress_Public,
		eConnectAddress_Failed,
	};
	EConnectAddress eConnectAddress = eConnectAddress_Default;
	while( eConnectAddress != eConnectAddress_Failed )
	{
		bool bConnected = false;
		int nRetries = 0;
		while (nRetries >= 0)
		{
			// If successful, then we're done.
			m_nLastConnectionResult = g_pLTClient->StartGame( const_cast< StartGameRequest * >( &m_StartGameRequest ));
			if( m_nLastConnectionResult == LT_OK )
			{
				bConnected = true;
				break;
			}

			// If we didn't timeout, then there's no reason to try again.
			if( m_nLastConnectionResult != LT_TIMEOUT )
			{
				break;
			}

			// Wait a little and try again.
			Sleep(250);
			nRetries--;
		}

		// Check if we connected.
		if( bConnected )
		{
			m_eConnectionState = eConnectionState_Connected;
			break;
		}

		// Go to the next address to try.
		if( eConnectAddress == eConnectAddress_Default )
		{
			strncpy( m_StartGameRequest.m_TCPAddress, m_sConnectPrivateAddress.c_str( ), MAX_SGR_STRINGLEN);
			eConnectAddress = eConnectAddress_Private;
		}
		else if( eConnectAddress == eConnectAddress_Private )
		{
			strncpy( m_StartGameRequest.m_TCPAddress, m_sConnectPublicAddress.c_str( ), MAX_SGR_STRINGLEN);
			eConnectAddress = eConnectAddress_Public;
		}
		else
		{
			eConnectAddress = eConnectAddress_Failed;
		}
	}

	if( eConnectAddress == eConnectAddress_Failed )
	{
		m_eConnectionState = eConnectionState_Failure;
	}

	// Make sure we throw away the serverbrowser objects.  Throw away
	// after startgame call just in case more comm was required.
	TermBrowsers( );
}