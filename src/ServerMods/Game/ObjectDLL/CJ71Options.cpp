// ---------------------------------------------------------------------------
//  CJ71Options.cpp -- see CJ71Options.h
//
//  Layout of the VC7.1 image (matches ClientMultiplayerMgr.cpp, namespace CJ71):
//    std::string  : 28 bytes  [0] allocator, [4] inline buffer or heap pointer,
//                             [20] size, [24] reserve (>= 16 means heap)
//    std::set<string> : 12 bytes  [4] head node, [8] size
//    set node     : _Left(0) _Parent(4) _Right(8) string(12..39) _Color(40) _Isnil(41)
//    BaseGameOptions : 0x94 bytes, starting with a vtable pointer
// ---------------------------------------------------------------------------

#include "stdafx.h"
#include "CJ71Options.h"
#include <string.h>

namespace
{
	const uint32 kMaxStringLen = 0x100000;
	const uint32 kMaxSetEntries = 4096;

	uint32 GetU32( const char* b, int o ) { uint32 v; memcpy( &v, b + o, 4 ); return v; }
	uint16 GetU16( const char* b, int o ) { uint16 v; memcpy( &v, b + o, 2 ); return v; }
	uint8  GetU8 ( const char* b, int o ) { return (uint8)b[o]; }
	float  GetF32( const char* b, int o ) { float v; memcpy( &v, b + o, 4 ); return v; }
	bool   GetBool( const char* b, int o ) { return b[o] != 0; }

	std::string GetStr( const char* b, int o )
	{
		const char* p = b + o;
		uint32 nSize = GetU32( p, 20 );
		uint32 nReserve = GetU32( p, 24 );
		if( nSize > kMaxStringLen )
			return std::string( );

		const char* pData = p + 4;
		if( nReserve >= 16 )
			memcpy( &pData, p + 4, 4 );
		if( !pData )
			return std::string( );
		return std::string( pData, nSize );
	}

	void WalkSet( const char* pNode, const char* pHead, StringSet& set, int& nBudget )
	{
		if( !pNode || pNode == pHead || nBudget <= 0 || pNode[41] )
			return;
		--nBudget;

		const char* pLeft; const char* pRight;
		memcpy( &pLeft, pNode + 0, 4 );
		memcpy( &pRight, pNode + 8, 4 );

		WalkSet( pLeft, pHead, set, nBudget );
		set.insert( GetStr( pNode, 12 ));
		WalkSet( pRight, pHead, set, nBudget );
	}

	void GetSet( const char* b, int o, StringSet& set )
	{
		set.clear( );
		const char* p = b + o;
		const char* pHead; memcpy( &pHead, p + 4, 4 );
		uint32 nSize = GetU32( p, 8 );
		if( !pHead || nSize == 0 || nSize > kMaxSetEntries )
			return;

		const char* pRoot; memcpy( &pRoot, pHead + 4, 4 );
		int nBudget = (int)nSize;
		WalkSet( pRoot, pHead, set, nBudget );
	}

	void GetBase( const char* b, int o, BaseGameOptions& g )
	{
		const char* p = b + o;
		g.m_sSessionName = GetStr( p, 0x04 );
		g.m_sCampaignName = GetStr( p, 0x20 );
		g.m_nMaxPlayers = GetU8( p, 0x3c );
		g.m_bFriendlyFire = GetBool( p, 0x3d );
		g.m_bWeaponsStay = GetBool( p, 0x3e );
		g.m_bUseSkills = GetBool( p, 0x3f );
		g.m_nDifficulty = GetU8( p, 0x40 );
		g.m_fPlayerDiffFactor = GetF32( p, 0x44 );
		g.m_nRunSpeed = GetU8( p, 0x48 );
		g.m_nScoreLimit = GetU8( p, 0x49 );
		g.m_nTimeLimit = GetU8( p, 0x4a );
		g.m_nRounds = GetU8( p, 0x4b );
		g.m_nFragScore = GetU8( p, 0x4c );
		g.m_nRevivingScore = GetU8( p, 0x4d );
		g.m_nNumTeams = GetU8( p, 0x4e );
		for( int i = 0; i < MAX_TEAMS; i++ )
		{
			g.m_nTeamModel[i] = GetU32( p, 0x50 + i * 4 );
			g.m_sTeamName[i] = GetStr( p, 0x58 + i * 28 );
		}
		g.m_nDeviceCompletedScore = GetU8( p, 0x90 );
		g.m_nLightPiecePlacedScore = GetU8( p, 0x91 );
		g.m_nHeavyPiecePlacedScore = GetU8( p, 0x92 );
		g.m_nPieceRemovedScore = GetU8( p, 0x93 );
	}
}

void CJ71::ReadImage( const char* b, ServerGameOptions& o )
{
	o.m_eGameType = (GameType)GetU32( b, 0x00 );
	o.m_bUsePassword = GetBool( b, 0x04 );
	o.m_sPassword = GetStr( b, 0x08 );
	o.m_sScmdPassword = GetStr( b, 0x24 );
	o.m_bAllowScmdCommands = GetBool( b, 0x40 );
	o.m_nPort = GetU16( b, 0x42 );
	o.m_nBandwidthServer = GetU8( b, 0x44 );
	o.m_nBandwidthServerCustom = GetU16( b, 0x46 );
	o.m_bLANOnly = GetBool( b, 0x48 );
	o.m_bDedicated = GetBool( b, 0x49 );
	GetSet( b, 0x4c, o.m_setRestrictedWeapons );
	GetSet( b, 0x58, o.m_setRestrictedAmmo );
	GetSet( b, 0x64, o.m_setRestrictedGear );
	o.m_sModName = GetStr( b, 0x70 );
	o.m_sProfileName = GetStr( b, 0x8c );
	o.m_bPreCacheAssets = GetBool( b, 0xa8 );
	o.m_bPerformanceTest = GetBool( b, 0xa9 );
	o.m_eServerStartResult = (EServerStartResult)GetU32( b, 0xac );

	GetBase( b, 0x0b0, o.GetSingleplayer( ));
	GetBase( b, 0x144, o.GetCoop( ));
	GetBase( b, 0x1d8, o.GetDeathmatch( ));
	GetBase( b, 0x26c, o.GetTeamDeathmatch( ));
	GetBase( b, 0x300, o.GetDemolition( ));
	GetBase( b, 0x394, o.GetDoomsday( ));
}

void CJ71::SetStartResult( char* pImage, EServerStartResult eResult )
{
	uint32 n = (uint32)eResult;
	memcpy( pImage + 0xac, &n, 4 );
}
