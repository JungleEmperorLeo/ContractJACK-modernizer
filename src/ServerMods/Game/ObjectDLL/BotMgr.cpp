// ----------------------------------------------------------------------- //
//
// MODULE  : BotMgr.cpp
//
// PURPOSE : Multiplayer bots - console commands and bot bookkeeping.
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "BotMgr.h"
#include "GameServerShell.h"
#include "GameStartPoint.h"
#include "PlayerObj.h"
#include "Spawner.h"
#include "ModelButeMgr.h"
#include "WeaponMgr.h"
#include "AIGoalButeMgr.h"
#include "AIButeMgr.h"
#include "BotNav.h"
#include "AIVolume.h"
#include "BotBrain.h"
#include "TeamMgr.h"
#include "MsgIDs.h"
#include "ServerUtilities.h"
#include <vector>

namespace
{
	typedef std::vector< LTObjRef > BotList;
	BotList	s_lstBots;
	uint32	s_nBotCounter = 0;
	uint32	s_nDesiredBots = 0;
	int		s_nBotSkill = 2;
	bool	s_bAdaptive = true;
	bool	s_bChat = true;
	int		s_nBotTeam = -1;	// -1 = auto-balance

	// Defaults used when bot-add is given no arguments.  Change at runtime with bot-default.
	char	s_szModel[64]	= "";
	char	s_szGoalSet[64]	= "";
	char	s_szWeapon[64]	= "";

	void PruneDeadRefs( )
	{
		BotList::iterator it = s_lstBots.begin( );
		while( it != s_lstBots.end( ))
		{
			if( !( HOBJECT )*it )
				it = s_lstBots.erase( it );
			else
				++it;
		}
	}

	bool GetSpawnTransform( HCLIENT hSender, LTVector& vPos, LTRotation& rRot )
	{
		GameStartPoint* pStartPt = g_pGameServerShell->FindStartPoint( LTNULL );
		if( pStartPt && pStartPt->m_hObject )
		{
			g_pLTServer->GetObjectPos( pStartPt->m_hObject, &vPos );
			g_pLTServer->GetObjectRotation( pStartPt->m_hObject, &rRot );
			pStartPt->SetLastUse( g_pLTServer->GetTime( ));
			return true;
		}

		// No start points - drop the bot in front of the player who asked.
		CPlayerObj* pPlayer = hSender ? ( CPlayerObj* )g_pLTServer->GetClientUserData( hSender ) : LTNULL;
		if( pPlayer && pPlayer->m_hObject )
		{
			g_pLTServer->GetObjectPos( pPlayer->m_hObject, &vPos );
			g_pLTServer->GetObjectRotation( pPlayer->m_hObject, &rRot );
			vPos += rRot.Forward( ) * 128.0f;
			return true;
		}

		return false;
	}

	void ListModels( )
	{
		if( !g_pModelButeMgr ) return;
		g_pLTServer->CPrint( "Models (%d):", g_pModelButeMgr->GetNumModels( ));
		for( int i = 0; i < g_pModelButeMgr->GetNumModels( ); i++ )
			g_pLTServer->CPrint( "  %s", g_pModelButeMgr->GetModelName(( ModelId )i ));
	}

	void ListGoalSets( )
	{
		if( !g_pAIGoalButeMgr ) { g_pLTServer->CPrint( "AI goal butes not loaded." ); return; }
		g_pLTServer->CPrint( "Goal sets (%d):", g_pAIGoalButeMgr->GetNumGoalSets( ));
		for( uint32 i = 0; i < g_pAIGoalButeMgr->GetNumGoalSets( ); i++ )
			g_pLTServer->CPrint( "  %s", g_pAIGoalButeMgr->GetGoalSet( i )->szName );
	}

	void ListWeapons( )
	{
		if( !g_pWeaponMgr ) return;
		g_pLTServer->CPrint( "Weapons (%d):", g_pWeaponMgr->GetNumWeapons( ));
		for( int i = 0; i < g_pWeaponMgr->GetNumWeapons( ); i++ )
		{
			WEAPON const* pWeapon = g_pWeaponMgr->GetWeapon( i );
			if( pWeapon && pWeapon->szName )
				g_pLTServer->CPrint( "  %s%s", pWeapon->szName, g_pWeaponMgr->IsPlayerWeapon( i ) ? "" : "  (non-player)" );
		}
	}

	// Case-insensitive substring test.
	bool ContainsNoCase( const char* szHay, const char* szNeedle )
	{
		if( !szHay || !szNeedle ) return false;
		size_t nLen = strlen( szNeedle );
		for( ; *szHay; szHay++ )
			if( _strnicmp( szHay, szNeedle, nLen ) == 0 ) return true;
		return false;
	}

	// Pick sensible defaults when the user gives no names.
	const char* AutoModel( HCLIENT hSender )
	{
		if( !g_pModelButeMgr ) return "";
		static const char* s_aszPrefer[] = { "Soldier", "Guard", "Merc", "Thug", "Henchman", "Ninja", "Agent" };
		for( int p = 0; p < sizeof( s_aszPrefer ) / sizeof( s_aszPrefer[0] ); p++ )
			for( int i = 0; i < g_pModelButeMgr->GetNumModels( ); i++ )
			{
				const char* szName = g_pModelButeMgr->GetModelName(( ModelId )i );
				if( ContainsNoCase( szName, s_aszPrefer[p] )) return szName;
			}
		// Fall back to the host's own model.
		CPlayerObj* pPlayer = hSender ? ( CPlayerObj* )g_pLTServer->GetClientUserData( hSender ) : LTNULL;
		if( pPlayer ) return g_pModelButeMgr->GetModelName( pPlayer->GetModelId( ));
		return "";
	}

	const char* AutoGoalSet( )
	{
		if( !g_pAIGoalButeMgr ) return "";
		static const char* s_aszPrefer[] = { "Soldier", "Attack", "Guard", "Merc", "Assassin" };
		for( int p = 0; p < sizeof( s_aszPrefer ) / sizeof( s_aszPrefer[0] ); p++ )
			for( uint32 i = 0; i < g_pAIGoalButeMgr->GetNumGoalSets( ); i++ )
			{
				const char* szName = g_pAIGoalButeMgr->GetGoalSet( i )->szName;
				if( ContainsNoCase( szName, s_aszPrefer[p] )) return szName;
			}
		return "";
	}

	const char* AutoWeapon( )
	{
		if( !g_pWeaponMgr ) return "";
		static const char* s_aszPrefer[] = { "AK", "Rifle", "SMG", "Pistol" };
		for( int p = 0; p < sizeof( s_aszPrefer ) / sizeof( s_aszPrefer[0] ); p++ )
			for( int i = 0; i < g_pWeaponMgr->GetNumWeapons( ); i++ )
			{
				WEAPON const* pWeapon = g_pWeaponMgr->GetWeapon( i );
				if( pWeapon && pWeapon->szName && g_pWeaponMgr->IsPlayerWeapon( i ) && ContainsNoCase( pWeapon->szName, s_aszPrefer[p] ))
					return pWeapon->szName;
			}
		return "";
	}

	// A brain is required: CAIHuman::ReadProp only creates one when the Brain property is
	// set, and SetGoalSet dereferences it.  Use the goal set's required brain if it has one.
	const char* AutoBrain( const char* szGoalSet )
	{
		if( !g_pAIButeMgr || g_pAIButeMgr->GetNumBrains( ) <= 0 ) return "Default";
		if( g_pAIGoalButeMgr && szGoalSet && szGoalSet[0] )
		{
			AIGBM_GoalSet* pGoalSet = g_pAIGoalButeMgr->GetGoalSet( szGoalSet );
			if( pGoalSet && !pGoalSet->lstRequiredBrains.empty( ))
			{
				int iBrain = *pGoalSet->lstRequiredBrains.begin( );
				if( iBrain >= 0 && iBrain < g_pAIButeMgr->GetNumBrains( ))
					return g_pAIButeMgr->GetBrain( iBrain )->szName;
			}
		}
		static const char* s_aszPrefer[] = { "Soldier", "Default" };
		for( int p = 0; p < sizeof( s_aszPrefer ) / sizeof( s_aszPrefer[0] ); p++ )
			for( int i = 0; i < g_pAIButeMgr->GetNumBrains( ); i++ )
				if( ContainsNoCase( g_pAIButeMgr->GetBrain( i )->szName, s_aszPrefer[p] ))
					return g_pAIButeMgr->GetBrain( i )->szName;
		return g_pAIButeMgr->GetBrain( 0 )->szName;
	}

	void AddBot( HCLIENT hSender, const char* szModel, const char* szGoalSet, const char* szWeapon )
	{
		LTVector vPos;
		LTRotation rRot;
		if( !GetSpawnTransform( hSender, vPos, rRot ))
		{
			g_pLTServer->CPrint( "bot-add: no start point to spawn at." );
			return;
		}

		char szName[32];
		sprintf( szName, "Bot%u", ++s_nBotCounter );

		char szSpawn[1024];
		int n = sprintf( szSpawn, "CAIHuman Name %s;MoveToFloor 1;UseDefaultAttachments 1;Brain %s", szName, AutoBrain( szGoalSet ));
		if( szModel && szModel[0] )
			n += sprintf( szSpawn + n, ";ModelTemplate %s", szModel );
		if( szGoalSet && szGoalSet[0] && g_pAIGoalButeMgr && g_pAIGoalButeMgr->GetGoalSetIndex( szGoalSet ) == ( uint32 )-1 )
		{
			g_pLTServer->CPrint( "bot-add: unknown goal set '%s' - spawning without one.", szGoalSet );
			szGoalSet = "";
		}
		if( szGoalSet && szGoalSet[0] )
			n += sprintf( szSpawn + n, ";GoalSet %s", szGoalSet );
		if( szWeapon && szWeapon[0] )
			n += sprintf( szSpawn + n, ";RightHand %s", szWeapon );

		g_pLTServer->CPrint( "bot-add: spawning '%s' at <%.0f,%.0f,%.0f>", szSpawn, VEC_EXPAND( vPos ));

		if( !g_pLTServer->GetClass( "CAIHuman" ))
		{
			g_pLTServer->CPrint( "bot-add: class CAIHuman is not registered in this server." );
			return;
		}

		BaseClass* pBot = SpawnObject( szSpawn, vPos, rRot );
		if( !pBot || !pBot->m_hObject )
		{
			g_pLTServer->CPrint( "bot-add: spawn FAILED (check the model/goalset/weapon names with bot-models, bot-goalsets, bot-weapons)." );
			return;
		}

		s_lstBots.push_back( LTObjRef( pBot->m_hObject ));
		g_pLTServer->CPrint( "bot-add: %s spawned (%u bots).", szName, ( uint32 )s_lstBots.size( ));
	}

	void KickBots( )
	{
		PruneDeadRefs( );
		uint32 nCount = s_lstBots.size( );
		for( BotList::iterator it = s_lstBots.begin( ); it != s_lstBots.end( ); ++it )
		{
			HOBJECT hBot = *it;
			if( hBot )
				g_pLTServer->RemoveObject( hBot );
		}
		s_lstBots.clear( );
		g_pLTServer->CPrint( "bot-kick: removed %u bots.", nCount );
	}

	// Move every bot to a spot in front of the player who asked (for testing).
	void BringBots( HCLIENT hSender )
	{
		PruneDeadRefs( );
		CPlayerObj* pPlayer = hSender ? ( CPlayerObj* )g_pLTServer->GetClientUserData( hSender ) : LTNULL;
		if( !pPlayer || !pPlayer->m_hObject ) return;
		LTVector vPos;
		LTRotation rRot;
		g_pLTServer->GetObjectPos( pPlayer->m_hObject, &vPos );
		g_pLTServer->GetObjectRotation( pPlayer->m_hObject, &rRot );
		LTVector vForward = rRot.Forward( );
		vForward.y = 0.0f;
		if( vForward.x * vForward.x + vForward.z * vForward.z > 0.001f ) vForward.Normalize( );
		int i = 0;
		for( BotList::iterator it = s_lstBots.begin( ); it != s_lstBots.end( ); ++it, ++i )
		{
			HOBJECT hBot = *it;
			LTVector vDest = vPos + vForward * ( 150.0f + 80.0f * i );
			g_pLTServer->SetObjectPos( hBot, &vDest );
		}
		g_pLTServer->CPrint( "bot-bring: moved %u bots in front of you.", ( uint32 )s_lstBots.size( ));
	}

	void ListBots( )
	{
		{
			uint32 nPB = 0;
			CPlayerObj::PlayerObjList::const_iterator itp = CPlayerObj::GetPlayerObjList( ).begin( );
			for( ; itp != CPlayerObj::GetPlayerObjList( ).end( ); ++itp )
			{
				CPlayerObj* p = *itp;
				if( !p || !p->IsBot( )) continue;
				LTVector vP;
				g_pLTServer->GetObjectPos( p->m_hObject, &vP );
				g_pLTServer->CPrint( "  %s (id %u) <%.0f,%.0f,%.0f> %s health %.0f frags %d", p->GetNetUniqueName( ), p->GetPlayerClientID( ),
					VEC_EXPAND( vP ), p->GetState( ) == PS_ALIVE ? "alive" : "dead", p->BotGetHealth( ), p->GetPlayerScore( )->GetFrags( ));
				nPB++;
			}
			g_pLTServer->CPrint( "Player bots: %u (wanted %u, skill %d)", nPB, s_nDesiredBots, s_nBotSkill );
		}

		PruneDeadRefs( );
		g_pLTServer->CPrint( "Bots (%u):", ( uint32 )s_lstBots.size( ));
		for( BotList::iterator it = s_lstBots.begin( ); it != s_lstBots.end( ); ++it )
		{
			HOBJECT hBot = *it;
			LTVector vPos;
			g_pLTServer->GetObjectPos( hBot, &vPos );
			uint32 dwFlags = 0;
			g_pCommonLT->GetObjectFlags( hBot, OFT_Flags, dwFlags );
			CCharacter* pChar = ( CCharacter* )g_pLTServer->HandleToObject( hBot );
			AIVolume* pVolume = ( pChar && pChar->HasLastVolume( )) ? pChar->GetLastVolume( ) : LTNULL;
			g_pLTServer->CPrint( "  %s <%.0f,%.0f,%.0f> %s volume:%s", GetObjectName( hBot ), VEC_EXPAND( vPos ),
				( dwFlags & FLAG_VISIBLE ) ? "visible" : "HIDDEN", pVolume ? pVolume->GetName( ) : "none" );
		}
	}

	// Writes every model / goal set / brain / weapon name to BotNav\botinfo.txt
	// (the console is too short to show long lists).
	void DumpInfo( )
	{
		CreateDirectoryA( "BotNav", NULL );
		FILE* fp = fopen( "BotNav\\botinfo.txt", "wt" );
		if( !fp ) { g_pLTServer->CPrint( "bot-dumpinfo: could not write BotNav\\botinfo.txt" ); return; }
		if( g_pModelButeMgr )
		{
			fprintf( fp, "[Models]\n" );
			for( int i = 0; i < g_pModelButeMgr->GetNumModels( ); i++ )
				fprintf( fp, "%s\n", g_pModelButeMgr->GetModelName(( ModelId )i ));
		}
		if( g_pAIGoalButeMgr )
		{
			fprintf( fp, "\n[GoalSets]\n" );
			for( uint32 i = 0; i < g_pAIGoalButeMgr->GetNumGoalSets( ); i++ )
			{
				AIGBM_GoalSet* pSet = g_pAIGoalButeMgr->GetGoalSet( i );
				fprintf( fp, "%s", pSet->szName );
				for( size_t b = 0; b < pSet->lstRequiredBrains.size( ); b++ )
				{
					int iBrain = pSet->lstRequiredBrains[b];
					if( g_pAIButeMgr && iBrain >= 0 && iBrain < g_pAIButeMgr->GetNumBrains( ))
						fprintf( fp, "%s%s", b ? "," : "  (requires brain: ", g_pAIButeMgr->GetBrain( iBrain )->szName );
				}
				fprintf( fp, "%s\n", pSet->lstRequiredBrains.empty( ) ? "" : ")" );
			}
		}
		if( g_pAIButeMgr )
		{
			fprintf( fp, "\n[Brains]\n" );
			for( int i = 0; i < g_pAIButeMgr->GetNumBrains( ); i++ )
				fprintf( fp, "%s\n", g_pAIButeMgr->GetBrain( i )->szName );
			fprintf( fp, "\n[AttributeTemplates]\n" );
			for( int i = 0; i < g_pAIButeMgr->GetNumTemplates( ); i++ )
				fprintf( fp, "%s\n", g_pAIButeMgr->GetTemplate( i )->szName );
		}
		if( g_pWeaponMgr )
		{
			fprintf( fp, "\n[Weapons]\n" );
			for( int i = 0; i < g_pWeaponMgr->GetNumWeapons( ); i++ )
			{
				WEAPON const* pWeapon = g_pWeaponMgr->GetWeapon( i );
				if( pWeapon && pWeapon->szName )
					fprintf( fp, "%s%s\n", pWeapon->szName, g_pWeaponMgr->IsPlayerWeapon( i ) ? "" : "  (non-player)" );
			}
		}
		fclose( fp );
		g_pLTServer->CPrint( "bot-dumpinfo: wrote BotNav\\botinfo.txt" );
	}

	// ================================================================= //
	// Player bots: real player objects driven by CBotBrain.
	// ================================================================= //

	const uint32 kFirstBotID	= 200;	// Scoreboard ids for bots (real clients use small ids).
	const uint32 kMaxBots		= 32;
	int		s_nLastCvarBots = 0;
	int		s_nLastCvarSkill = -1;
	float	s_fNextBotUpdate	= 0.0f;

	const char* s_aszBotNames[] =
	{
		"(BOT)Gordon", "(BOT)Duke", "(BOT)Delta", "(BOT)Cate", "(BOT)Joanna", "(BOT)Kyle", "(BOT)Sam", "(BOT)Chell",
		"(BOT)Corvo", "(BOT)Booker", "(BOT)Jack", "(BOT)Adam", "(BOT)Faith", "(BOT)Daniel", "(BOT)Philip", "(BOT)Havoc",
		"(BOT)Max Payne", "(BOT)Parker", "(BOT)Ranger", "(BOT)Soap", "(BOT)Blade", "(BOT)BJ", "(BOT)Gina", "(BOT)Strelok",
		"(BOT)Rex", "(BOT)Bitterman", "(BOT)Barney", "(BOT)Shephard", "(BOT)JC Denton", "(BOT)MT Foxtrot", "(BOT)Munroe", "(BOT)Fledgling"
	};
	const uint32 kNumBotNames = sizeof( s_aszBotNames ) / sizeof( s_aszBotNames[0] );

	uint32 CountPlayerBots( )
	{
		uint32 n = 0;
		CPlayerObj::PlayerObjList::const_iterator it = CPlayerObj::GetPlayerObjList( ).begin( );
		for( ; it != CPlayerObj::GetPlayerObjList( ).end( ); ++it )
			if( *it && ( *it )->IsBot( )) n++;
		return n;
	}

	CPlayerObj* FindPlayerByID( uint32 nID )
	{
		CPlayerObj::PlayerObjList::const_iterator it = CPlayerObj::GetPlayerObjList( ).begin( );
		for( ; it != CPlayerObj::GetPlayerObjList( ).end( ); ++it )
			if( *it && ( *it )->GetPlayerClientID( ) == nID ) return *it;
		return LTNULL;
	}

	bool NameInUse( const char* szName )
	{
		CPlayerObj::PlayerObjList::const_iterator it = CPlayerObj::GetPlayerObjList( ).begin( );
		for( ; it != CPlayerObj::GetPlayerObjList( ).end( ); ++it )
		{
			CPlayerObj* p = *it;
			if( !p || ( !p->GetClient( ) && !p->IsBot( ))) continue;
			const char* szOther = p->GetNetUniqueName( );
			if( szOther && _stricmp( szOther, szName ) == 0 ) return true;
		}
		return false;
	}

	ModelId PickBotModel( )
	{
		// Use the same models players choose from in deathmatch.
		std::vector<ModelId> aModels;
		for( int i = 0; i < g_pModelButeMgr->GetNumModels( ); i++ )
		{
			const char* szName = g_pModelButeMgr->GetModelName(( ModelId )i );
			if( ContainsNoCase( szName, "MultiPlayer_DM" ) || ContainsNoCase( szName, "_multi" ))
				aModels.push_back(( ModelId )i );
		}
		if( aModels.empty( ))
			return g_pModelButeMgr->GetModelId( "Player" );
		return aModels[GetRandom( 0, ( int )aModels.size( ) - 1 )];
	}

	CPlayerObj* AddPlayerBot( int nSkill, int nTeam = -1 )
	{
		if( !IsMultiplayerGame( ))
		{
			g_pLTServer->CPrint( "bot-add: bots only work in multiplayer games." );
			return LTNULL;
		}

		if( !BotNav::EnsureNav( ) || !BotNav::HasGraph( ))
		{
			g_pLTServer->CPrint( "bot-add: no navigation for this map, bots can't be added." );
			return LTNULL;
		}

		// Free id and name.
		uint32 nID = 0;
		for( uint32 i = 0; i < kMaxBots; i++ )
		{
			if( !FindPlayerByID( kFirstBotID + i )) { nID = kFirstBotID + i; break; }
		}
		if( !nID )
		{
			g_pLTServer->CPrint( "bot-add: too many bots." );
			return LTNULL;
		}

		char szName[64];
		szName[0] = 0;
		uint32 nStart = GetRandom( 0, kNumBotNames - 1 );
		for( uint32 i = 0; i < kNumBotNames; i++ )
		{
			const char* szTry = s_aszBotNames[( nStart + i ) % kNumBotNames];
			if( !NameInUse( szTry )) { SAFE_STRCPY( szName, szTry ); break; }
		}
		for( uint32 nCopy = 2; !szName[0] && nCopy < 100; nCopy++ )
		{
			for( uint32 i = 0; i < kNumBotNames; i++ )
			{
				char szTry[64];
				sprintf( szTry, "%s %u", s_aszBotNames[( nStart + i ) % kNumBotNames], nCopy );
				if( !NameInUse( szTry )) { SAFE_STRCPY( szName, szTry ); break; }
			}
		}
		if( !szName[0] )
			sprintf( szName, "Bot%u", nID - kFirstBotID + 1 );

		HCLASS hClass = g_pLTServer->GetClass( "CPlayerObj" );
		if( !hClass ) return LTNULL;

		ObjectCreateStruct ocs;
		INIT_OBJECTCREATESTRUCT( ocs );
		ocs.m_Rotation.Init( );
		ocs.m_Pos = LTVector( 0.0f, 0.0f, 0.0f );
		ocs.m_Flags = 0;
		ocs.m_UserData = PickBotModel( );

		CPlayerObj* pBot = ( CPlayerObj* )g_pLTServer->CreateObject( hClass, &ocs );
		if( !pBot )
		{
			g_pLTServer->CPrint( "bot-add: could not create the bot's player object." );
			return LTNULL;
		}

		{
			CAutoMessage cMsg;
			cMsg.Writeuint8( MID_PLAYER_REMOVED );
			cMsg.Writeuint32( nID );
			g_pLTServer->SendToClient( cMsg.Read( ), LTNULL, MESSAGE_GUARANTEED );
			if( IsTeamGameType( ))
				CTeamMgr::Instance( ).RemovePlayer( nID );
		}

		CBotBrain* pBrain = new CBotBrain( nSkill );
		const char* szStyle = pBrain->SetPersonality( szName );
		pBot->InitBot( nID, szName, pBrain );

		if( IsTeamGameType( ))
		{
			CTeam* pTeam = ( nTeam >= 0 && nTeam < ( int )CTeamMgr::Instance( ).GetNumTeams( ))
				? CTeamMgr::Instance( ).GetTeam(( uint8 )nTeam ) : LTNULL;
			if( !pTeam )
				pTeam = CTeamMgr::Instance( ).GetTeamWithLeastPlayers( );
			if( pTeam )
			{
				CTeamMgr::Instance( ).AddPlayer( nID, pTeam->GetID( ));
				CTeamMgr::Instance( ).UpdateClient( );
				pBot->SetupTeamModel( false );
			}
		}

		// Tell everyone a new player joined (same message as for a real player).
		g_pGameServerShell->SendPlayerInfoMsgToClients( LTNULL, pBot, MID_PI_JOIN );

		pBot->Respawn( );
		pBot->SetClientLoaded( true );
		pBrain->OnRespawn( pBot );
		pBrain->Greet( pBot );

		g_pLTServer->CPrint( "bot-add: %s joined (skill %d, %s%s).", szName, nSkill, szStyle, IsTeamGameType( ) ? ", team game" : "" );
		return pBot;
	}

	void RemovePlayerBot( CPlayerObj* pBot )
	{
		if( !pBot || !pBot->IsBot( )) return;
		uint32 nID = pBot->GetPlayerClientID( );

		CAutoMessage cMsg;
		cMsg.Writeuint8( MID_PLAYER_REMOVED );
		cMsg.Writeuint32( nID );
		g_pLTServer->SendToClient( cMsg.Read( ), LTNULL, MESSAGE_GUARANTEED );

		if( IsTeamGameType( ))
			CTeamMgr::Instance( ).RemovePlayer( nID );

		g_pLTServer->RemoveObject( pBot->m_hObject );
	}

	void KickPlayerBots( )
	{
		std::vector< CPlayerObj* > aBots;
		CPlayerObj::PlayerObjList::const_iterator it = CPlayerObj::GetPlayerObjList( ).begin( );
		for( ; it != CPlayerObj::GetPlayerObjList( ).end( ); ++it )
			if( *it && ( *it )->IsBot( )) aBots.push_back( *it );
		for( size_t i = 0; i < aBots.size( ); i++ )
			RemovePlayerBot( aBots[i] );
	}

	void PrintHelp( )
	{
		g_pLTServer->CPrint( "Bot commands (type them after 'cmd', e.g.  cmd bot-add):" );
		g_pLTServer->CPrint( "  bot-add [count] [skill] [team]   add player bots (skill 2 = normal, 3 = hard, 4 = expert; team 0/1, default balanced)" );
		g_pLTServer->CPrint( "  bot-skill [2-4]   skill for bots added from now on" );
		g_pLTServer->CPrint( "  bot-adapt [0|1]   bots adjust their skill to how the humans are doing" );
		g_pLTServer->CPrint( "  bot-chat [0|1]    bots talk and taunt in chat" );
		g_pLTServer->CPrint( "  bot-addai [model] [goalset] [weapon]   (test) single-player AI soldier" );
		g_pLTServer->CPrint( "  bot-default [model] [goalset] [weapon]   set the defaults for bot-add" );
		g_pLTServer->CPrint( "  bot-kick        remove all bots" );
		g_pLTServer->CPrint( "  bot-list        list bots" );
		g_pLTServer->CPrint( "  bot-bring       move all bots in front of you" );
		g_pLTServer->CPrint( "  bot-navinfo     how much navigation this map has" );
		g_pLTServer->CPrint( "  bot-navgen      generate navigation now and save it (map must have none loaded)" );
		g_pLTServer->CPrint( "  bot-navload     load the saved navigation (or generate it)" );
		g_pLTServer->CPrint( "  bot-navdelete   delete this map's saved navigation file" );
		g_pLTServer->CPrint( "  bot-navshow / bot-navhide   draw the navigation near you (run again after moving)" );
		g_pLTServer->CPrint( "  bot-models / bot-goalsets / bot-weapons   list valid names" );
	}
}

// Adaptive skill: when the best human is out-scoring the best bot the bots tighten
// up, when they are being beaten up they ease off (at most one level either way).
static void UpdateAdaptiveSkill( )
{
	if( !s_bAdaptive )
	{
		CBotBrain::SetAdaptiveSkill( 0.0f );
		return;
	}
	bool bHuman = false, bBot = false;
	int nHumanBest = 0, nBotBest = 0;
	CPlayerObj::PlayerObjList::const_iterator it = CPlayerObj::GetPlayerObjList( ).begin( );
	for( ; it != CPlayerObj::GetPlayerObjList( ).end( ); ++it )
	{
		CPlayerObj* p = *it;
		if( !p ) continue;
		int nF = p->GetPlayerScore( )->GetFrags( );
		if( p->IsBot( ))
		{
			if( !bBot || nF > nBotBest ) nBotBest = nF;
			bBot = true;
		}
		else if( p->GetClient( ) && p->IsClientLoaded( ))
		{
			if( !bHuman || nF > nHumanBest ) nHumanBest = nF;
			bHuman = true;
		}
	}
	if( !bHuman || !bBot )
	{
		CBotBrain::SetAdaptiveSkill( 0.0f );
		return;
	}
	float fTarget = ( float )( nHumanBest - nBotBest ) / 6.0f;
	if( fTarget > 1.0f ) fTarget = 1.0f;
	if( fTarget < -1.0f ) fTarget = -1.0f;
	float fCur = CBotBrain::GetAdaptiveSkill( );
	CBotBrain::SetAdaptiveSkill( fCur + ( fTarget - fCur ) * 0.08f );
}

void BotMgr::Clear( )
{
	s_lstBots.clear( );
}

void BotMgr::Update( )
{
	// The wanted bot count / skill are kept in console variables so they
	// survive a map change; a new level re-reads them and re-adds the bots.
	static bool s_bNavPrepared = false;
	if( g_pLTServer )
	{
		static bool s_bRestored = false;
		static float s_fPrevTime = -1.0f;
		static uint32 s_nSavedBots = ( uint32 )-1;
		static int s_nSavedSkill = -1;

		float fT = g_pLTServer->GetTime( );
		bool bNewLevel = ( s_fPrevTime >= 0.0f && fT < s_fPrevTime - 0.5f );
		s_fPrevTime = fT;

		if( !s_bRestored || bNewLevel )
		{
			s_bRestored = true;
			uint32 nBots = s_nDesiredBots;
			int nSkill = s_nBotSkill;
			HCONVAR hB = g_pLTServer->GetGameConVar( "BotDesired" );
			HCONVAR hS = g_pLTServer->GetGameConVar( "BotSkillCur" );
			if( hB ) nBots = ( uint32 )LTCLAMP(( int )g_pLTServer->GetVarValueFloat( hB ), 0, ( int )kMaxBots );
			if( hS ) nSkill = LTCLAMP(( int )g_pLTServer->GetVarValueFloat( hS ), 2, 4 );
			if( !nBots )
			{
				// Fall back to what the host menu last set.
				HCONVAR hMB = g_pLTServer->GetGameConVar( "BotCount" );
				HCONVAR hMS = g_pLTServer->GetGameConVar( "BotSkill" );
				if( hMB ) nBots = ( uint32 )LTCLAMP(( int )g_pLTServer->GetVarValueFloat( hMB ), 0, ( int )kMaxBots );
				if( hMS ) nSkill = LTCLAMP(( int )g_pLTServer->GetVarValueFloat( hMS ), 2, 4 );
			}
			s_nDesiredBots = nBots;
			s_nBotSkill = nSkill;
			{
				HCONVAR hA = g_pLTServer->GetGameConVar( "BotAdaptOff" );
				HCONVAR hC = g_pLTServer->GetGameConVar( "BotChatOff" );
				if( hA ) s_bAdaptive = g_pLTServer->GetVarValueFloat( hA ) == 0.0f;
				if( hC ) s_bChat = g_pLTServer->GetVarValueFloat( hC ) == 0.0f;
				CBotBrain::SetChatEnabled( s_bChat );
				CBotBrain::SetAdaptiveSkill( 0.0f );
			}
			s_nSavedBots = s_nDesiredBots;
			s_nSavedSkill = s_nBotSkill;
			s_fNextBotUpdate = 0.0f;
			s_bNavPrepared = false;
			if( bNewLevel )
				g_pLTServer->CPrint( "bots: new level, %u bots wanted (skill %d).", s_nDesiredBots, s_nBotSkill );
		}
		if( s_nSavedBots != s_nDesiredBots || s_nSavedSkill != s_nBotSkill )
		{
			char szVal[16];
			sprintf( szVal, "%u", s_nDesiredBots );
			g_pLTServer->SetGameConVar( "BotDesired", szVal );
			sprintf( szVal, "%d", s_nBotSkill );
			g_pLTServer->SetGameConVar( "BotSkillCur", szVal );
			s_nSavedBots = s_nDesiredBots;
			s_nSavedSkill = s_nBotSkill;
		}
	}

	if( !s_nDesiredBots || !IsMultiplayerGame( ) || !g_pGameServerShell || g_pGameServerShell->IsPaused( ))
		return;

	// Build / load the navigation as soon as the level is up, while the player
	// is still on the loading screen, instead of in the middle of the first
	// frames of the match (that was a visible hitch).
	if( !s_bNavPrepared )
	{
		s_bNavPrepared = true;
		BotNav::EnsureNav( );
		BotNav::BuildLadders( );
	}

	float fNow = g_pLTServer->GetTime( );
	if( fNow < s_fNextBotUpdate )
		return;
	s_fNextBotUpdate = fNow + 0.5f;

	// Only once a real player is actually in the level (not mid map change).
	bool bHumanInGame = false;
	CPlayerObj::PlayerObjList::const_iterator it = CPlayerObj::GetPlayerObjList( ).begin( );
	for( ; it != CPlayerObj::GetPlayerObjList( ).end( ); ++it )
	{
		if( *it && ( *it )->GetClient( ) && ( *it )->RespawnCalled( ) && ( *it )->IsClientLoaded( ))
		{
			bHumanInGame = true;
			break;
		}
	}
	if( !bHumanInGame )
		return;

	UpdateAdaptiveSkill( );

	uint32 nBots = CountPlayerBots( );
	if( nBots < s_nDesiredBots )
	{
		if( !AddPlayerBot( s_nBotSkill, s_nBotTeam ))
			s_fNextBotUpdate = fNow + 4.0f;	// Don't spam if it can't work on this map.
	}
	else if( nBots > s_nDesiredBots )
	{
		it = CPlayerObj::GetPlayerObjList( ).begin( );
		for( ; it != CPlayerObj::GetPlayerObjList( ).end( ); ++it )
		{
			if( *it && ( *it )->IsBot( ))
			{
				RemovePlayerBot( *it );
				break;
			}
		}
	}
}

bool BotMgr::HandleConsoleCommand( HCLIENT hSender, const char* szCommand )
{
	if( !szCommand || _strnicmp( szCommand, "bot", 3 ) != 0 )
		return false;

	ConParse parse;
	parse.Init( szCommand );
	if( g_pCommonLT->Parse( &parse ) != LT_OK || parse.m_nArgs < 1 )
		return false;

	const char* szVerb = parse.m_Args[0];
	if( _strnicmp( szVerb, "bot-", 4 ) != 0 )
		return false;

	// Only the host may control bots.
	if( hSender && !( g_pLTServer->GetClientInfoFlags( hSender ) & CIF_LOCAL ))
	{
		g_pLTServer->CPrint( "%s: only the host can use bot commands.", szVerb );
		return true;
	}

	const char* a1 = parse.m_nArgs > 1 ? parse.m_Args[1] : LTNULL;
	const char* a2 = parse.m_nArgs > 2 ? parse.m_Args[2] : LTNULL;
	const char* a3 = parse.m_nArgs > 3 ? parse.m_Args[3] : LTNULL;

	if( _stricmp( szVerb, "bot-add" ) == 0 )
	{
		// bot-add [count] [skill 0-3] [team 0/1]
		s_nBotTeam = a3 ? LTCLAMP( atoi( a3 ), 0, 7 ) : -1;
		int nCount = a1 ? atoi( a1 ) : 1;
		if( nCount < 1 ) nCount = 1;
		if( a2 ) s_nBotSkill = LTCLAMP( atoi( a2 ), 2, 4 );
		s_nDesiredBots = CountPlayerBots( ) + ( uint32 )nCount;
		if( s_nDesiredBots > kMaxBots ) s_nDesiredBots = kMaxBots;
		// Add the first one right away; BotMgr::Update adds the rest (and re-adds
		// bots after a map change).
		if( AddPlayerBot( s_nBotSkill, s_nBotTeam ))
			s_fNextBotUpdate = g_pLTServer->GetTime( ) + 0.5f;
	}
	else if( _stricmp( szVerb, "bot-config" ) == 0 )
	{
		// bot-config <count> <skill 1-3>: what the host menu's Bots / Bot Skill set.
		int nCount = a1 ? LTCLAMP( atoi( a1 ), 0, ( int )kMaxBots ) : ( int )s_nDesiredBots;
		int nSkill = a2 ? LTCLAMP( atoi( a2 ), 2, 4 ) : s_nBotSkill;
		bool bSkillChanged = ( nSkill != s_nBotSkill );
		s_nDesiredBots = ( uint32 )nCount;
		s_nBotSkill = nSkill;
		s_nBotTeam = -1;
		if( bSkillChanged )
			KickPlayerBots( );		// Re-added at the new skill by BotMgr::Update.
		s_fNextBotUpdate = 0.0f;
		g_pLTServer->CPrint( "bot-config: %d bots, skill %d.", nCount, nSkill );
	}
	else if( _stricmp( szVerb, "bot-remove" ) == 0 )
	{
		// Remove the most recently added player bot.
		CPlayerObj* pLast = LTNULL;
		CPlayerObj::PlayerObjList::const_iterator itb = CPlayerObj::GetPlayerObjList( ).begin( );
		for( ; itb != CPlayerObj::GetPlayerObjList( ).end( ); ++itb )
			if( *itb && ( *itb )->IsBot( )) pLast = *itb;
		if( pLast )
			RemovePlayerBot( pLast );
		s_nDesiredBots = CountPlayerBots( );
	}
	else if( _stricmp( szVerb, "bot-adapt" ) == 0 )
	{
		if( a1 ) s_bAdaptive = atoi( a1 ) != 0;
		g_pLTServer->SetGameConVar( "BotAdaptOff", s_bAdaptive ? "0" : "1" );
		if( !s_bAdaptive ) CBotBrain::SetAdaptiveSkill( 0.0f );
		g_pLTServer->CPrint( "bot-adapt: %s (bots %s to how well the humans are doing).", s_bAdaptive ? "on" : "off", s_bAdaptive ? "adjust their skill" : "keep a fixed skill" );
	}
	else if( _stricmp( szVerb, "bot-chat" ) == 0 )
	{
		if( a1 ) s_bChat = atoi( a1 ) != 0;
		g_pLTServer->SetGameConVar( "BotChatOff", s_bChat ? "0" : "1" );
		CBotBrain::SetChatEnabled( s_bChat );
		g_pLTServer->CPrint( "bot-chat: %s.", s_bChat ? "on" : "off" );
	}
	else if( _stricmp( szVerb, "bot-skill" ) == 0 )
	{
		if( a1 ) s_nBotSkill = LTCLAMP( atoi( a1 ), 2, 4 );
		g_pLTServer->CPrint( "bot-skill: %d (2 = normal, 3 = hard, 4 = expert). Applies to bots added from now on.", s_nBotSkill );
	}
	else if( _stricmp( szVerb, "bot-addai" ) == 0 )
	{
		// Old test path: a single-player AI soldier.
		BotNav::EnsureNav( );

		const char* szModel   = a1 ? a1 : ( s_szModel[0]   ? s_szModel   : AutoModel( hSender ));
		const char* szGoalSet = a2 ? a2 : ( s_szGoalSet[0] ? s_szGoalSet : AutoGoalSet( ));
		const char* szWeapon  = a3 ? a3 : ( s_szWeapon[0]  ? s_szWeapon  : AutoWeapon( ));
		AddBot( hSender, szModel, szGoalSet, szWeapon );
	}
	else if( _stricmp( szVerb, "bot-default" ) == 0 )
	{
		if( a1 ) SAFE_STRCPY( s_szModel, a1 );
		if( a2 ) SAFE_STRCPY( s_szGoalSet, a2 );
		if( a3 ) SAFE_STRCPY( s_szWeapon, a3 );
		g_pLTServer->CPrint( "bot-default: model '%s' goalset '%s' weapon '%s'", s_szModel, s_szGoalSet, s_szWeapon );
	}
	else if( _stricmp( szVerb, "bot-kick" ) == 0 )
	{
		s_nDesiredBots = 0;
		KickPlayerBots( );
		KickBots( );
	}
	else if( _stricmp( szVerb, "bot-list" ) == 0 )		ListBots( );
	else if( _stricmp( szVerb, "bot-bring" ) == 0 )		BringBots( hSender );
	else if( _stricmp( szVerb, "bot-navgen" ) == 0 )	BotNav::Generate( true );
	else if( _stricmp( szVerb, "bot-navload" ) == 0 )	BotNav::EnsureNav( );
	else if( _stricmp( szVerb, "bot-navinfo" ) == 0 )	BotNav::PrintInfo( );
	else if( _stricmp( szVerb, "bot-navdelete" ) == 0 )	BotNav::DeleteCache( );
	else if( _stricmp( szVerb, "bot-navshow" ) == 0 || _stricmp( szVerb, "bot-navhide" ) == 0 )
	{
		LTVector vPos( 0.0f, 0.0f, 0.0f );
		CPlayerObj* pPlayer = hSender ? ( CPlayerObj* )g_pLTServer->GetClientUserData( hSender ) : LTNULL;
		if( pPlayer && pPlayer->m_hObject )
			g_pLTServer->GetObjectPos( pPlayer->m_hObject, &vPos );
		BotNav::Show( _stricmp( szVerb, "bot-navshow" ) == 0, vPos );
	}
	else if( _stricmp( szVerb, "bot-models" ) == 0 )	ListModels( );
	else if( _stricmp( szVerb, "bot-goalsets" ) == 0 )	ListGoalSets( );
	else if( _stricmp( szVerb, "bot-weapons" ) == 0 )	ListWeapons( );
	else if( _stricmp( szVerb, "bot-dumpinfo" ) == 0 )	DumpInfo( );
	else PrintHelp( );

	return true;
}
