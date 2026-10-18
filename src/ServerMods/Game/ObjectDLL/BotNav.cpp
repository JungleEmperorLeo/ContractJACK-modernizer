// ----------------------------------------------------------------------- //
//
// MODULE  : BotNav.cpp
//
// PURPOSE : Automatic navigation (AIVolume) generation for multiplayer bots.
//           See BotNav.h.
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "BotNav.h"
#include "AIVolume.h"
#include "AIVolumeMgr.h"
#include "GameServerShell.h"
#include "GameStartPoint.h"
#include "LiteObjectMgr.h"
#include "ServerMissionMgr.h"
#include "ServerUtilities.h"
#include "CommonUtilities.h"
#include "DebugLineSystem.h"
#include <vector>
#include <map>
#include <deque>
#include <string>
#include <stdio.h>
#include <math.h>
#include <queue>
#include <functional>
#include <algorithm>

namespace
{
	// Grid / movement constants (world units).
	const int	kCell			= 64;	// Cell size.  Volume connections must be >= 48 units.
	const int	kHalf			= kCell / 2;
	const float	kStep			= 36.0f;	// Max floor height change between neighbouring cells.
	const float	kProbeUp		= 48.0f;	// Floor probes start this far above the reference height.
	const float	kProbeDown		= 96.0f;	// ...and go this far below it.
	const float	kFootprint		= 20.0f;	// Extra floor probes this far from the cell centre.
	const float	kFootprintDY	= 20.0f;	// ...must be within this much of the centre floor.
	const float	kHeadroom		= 72.0f;	// Clear space needed above the floor.
	const int	kSlabBelow		= 16;	// Volume extends this far below the floor...
	const int	kSlabAbove		= 72;	// ...and this far above it.
	const uint32 kMaxNodes		= 40000;
	const int	kFileVersion = 4;	// 2: stores the walk graph (cells + links)

	struct Node
	{
		int ix, iz;
		int y;				// Floor height (rounded).
		int ox, oz;			// Where in the cell the node really is (offset from the centre).
		int aEdge[4];		// Walkable neighbour node per direction, or -1.
	};

	const int s_aDirX[4] = { 1, -1, 0, 0 };
	const int s_aDirZ[4] = { 0, 0, 1, -1 };
	const int s_aOpposite[4] = { 1, 0, 3, 2 };

	// Points tried inside a cell when its centre isn't standable (small rooms,
	// furniture, off-grid doorways).
	const int s_aSample[13][2] = { { 0, 0 }, { 16, 0 }, { -16, 0 }, { 0, 16 }, { 0, -16 },
		{ 16, 16 }, { -16, 16 }, { 16, -16 }, { -16, -16 }, { 24, 0 }, { -24, 0 }, { 0, 24 }, { 0, -24 } };

	struct Box
	{
		LTVector vCenter;
		LTVector vDims;
	};

	// ----------------------------------------------------------------- //
	// Ray casting.
	// ----------------------------------------------------------------- //

	bool NavFilterFn( HOBJECT hObj, void* /*pUserData*/ )
	{
		if( IsMainWorld( hObj ))
			return true;

		if( OT_WORLDMODEL == GetObjectType( hObj ))
		{
			// Doors open for AI, so don't treat them as walls.
			static HCLASS s_hDoor = g_pLTServer->GetClass( "Door" );
			if( s_hDoor && g_pLTServer->IsKindOf( g_pLTServer->GetObjectClass( hObj ), s_hDoor ))
				return false;
			return true;
		}

		return false;
	}

	// Like NavFilterFn but solid props (desks, crates, shelves) count too.  Used for
	// clearance / walkability tests, never for finding floors.
	bool NavPropFilterFn( HOBJECT hObj, void* pUserData )
	{
		if( NavFilterFn( hObj, pUserData )) return true;
		return ( OT_MODEL == GetObjectType( hObj ) && IsKindOf( hObj, "Prop" )) ? true : false;
	}

	bool RayP( const LTVector& vFrom, const LTVector& vTo, IntersectInfo* pInfo )
	{
		IntersectQuery iq;
		IntersectInfo ii;
		iq.m_From = vFrom;
		iq.m_To = vTo;
		iq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID | INTERSECT_HPOLY;
		iq.m_FilterFn = NavPropFilterFn;
		return !!g_pLTServer->IntersectSegment( &iq, pInfo ? pInfo : &ii );
	}

	bool Ray( const LTVector& vFrom, const LTVector& vTo, IntersectInfo* pInfo )
	{
		IntersectQuery iq;
		IntersectInfo ii;
		iq.m_From = vFrom;
		iq.m_To = vTo;
		iq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID | INTERSECT_HPOLY;
		iq.m_FilterFn = NavFilterFn;
		bool bHit = !!g_pLTServer->IntersectSegment( &iq, pInfo ? pInfo : &ii );
		return bHit;
	}

	// Finds walkable floor straight down between yTop and yBottom.
	bool ProbeFloor( float x, float z, float yTop, float yBottom, float* pY )
	{
		IntersectInfo ii;
		if( !Ray( LTVector( x, yTop, z ), LTVector( x, yBottom, z ), &ii ))
			return false;
		if( ii.m_Plane.Normal( ).y < 0.7f )
			return false;	// Wall, steep slope or ceiling underside.
		*pY = ii.m_Point.y;
		return true;
	}

	// ----------------------------------------------------------------- //
	// Generator state.
	// ----------------------------------------------------------------- //

	typedef std::map< long long, std::vector<int> > CellMap;

	struct Generator
	{
		std::vector<Node>	aNodes;
		CellMap				mapCells;
		float				fOriginX, fOriginZ;
		int					nCellsX, nCellsZ;

		static long long Key( int ix, int iz ) { return ( ( long long )ix << 32 ) | ( uint32 )iz; }

		float CenterX( int ix ) const { return fOriginX + ( float )( ix * kCell + kHalf ); }
		float CenterZ( int iz ) const { return fOriginZ + ( float )( iz * kCell + kHalf ); }

		int FindNode( int ix, int iz, float y ) const
		{
			CellMap::const_iterator it = mapCells.find( Key( ix, iz ));
			if( it == mapCells.end( )) return -1;
			for( size_t i = 0; i < it->second.size( ); i++ )
			{
				const Node& n = aNodes[it->second[i]];
				if( fabs(( float )n.y - y ) <= kStep )
					return it->second[i];
			}
			return -1;
		}

		// Is this cell standable near height yRef?  Returns the floor height.
		bool CellOk( int ix, int iz, float yRef, float* pY, int ox = 0, int oz = 0 ) const
		{
			if( ix < 0 || iz < 0 || ix >= nCellsX || iz >= nCellsZ )
				return false;

			float cx = CenterX( ix ) + ( float )ox, cz = CenterZ( iz ) + ( float )oz;
			float y;
			if( !ProbeFloor( cx, cz, yRef + kProbeUp, yRef - kProbeDown, &y ))
				return false;
			if( fabs( y - yRef ) > kStep )
				return false;

			// The cell must be mostly floor (no cliff edges right next to the centre).
			static const float s_aOff[4][2] = { { kFootprint, 0 }, { -kFootprint, 0 }, { 0, kFootprint }, { 0, -kFootprint } };
			for( int i = 0; i < 4; i++ )
			{
				float y2;
				if( !ProbeFloor( cx + s_aOff[i][0], cz + s_aOff[i][1], y + kProbeUp, y - kProbeDown, &y2 ))
					return false;
				if( fabs( y2 - y ) > kFootprintDY )
					return false;
			}

			// Room to stand.
			if( RayP( LTVector( cx, y + 2.0f, cz ), LTVector( cx, y + kHeadroom, cz ), LTNULL ))
				return false;

			*pY = y;
			return true;
		}

		// Can a character walk between the centres of two cells?
		bool EdgeOk( float ax, float az, float yA, float bx, float bz, float yB ) const
		{
			if( fabs( yA - yB ) > kStep )
				return false;

			float yHigh = ( yA > yB ) ? yA : yB;
			float yLow = ( yA < yB ) ? yA : yB;

			// Nothing in the way at step-over and chest height.
			if( RayP( LTVector( ax, yHigh + 20.0f, az ), LTVector( bx, yHigh + 20.0f, bz ), LTNULL ))
				return false;
			if( RayP( LTVector( ax, yHigh + 56.0f, az ), LTVector( bx, yHigh + 56.0f, bz ), LTNULL ))
				return false;

			// Floor between the two (no gap/pit).
			float yMid;
			if( !ProbeFloor(( ax + bx ) * 0.5f, ( az + bz ) * 0.5f, yHigh + kProbeUp, yLow - kProbeDown, &yMid ))
				return false;
			if( yMid < yLow - 8.0f || yMid > yHigh + 8.0f )
				return false;

			return true;
		}

		float NodeX( const Node& n ) const { return CenterX( n.ix ) + ( float )n.ox; }
		float NodeZ( const Node& n ) const { return CenterZ( n.iz ) + ( float )n.oz; }

		int AddNode( int ix, int iz, float y, int ox = 0, int oz = 0 )
		{
			Node n;
			n.ix = ix;
			n.iz = iz;
			n.ox = ox;
			n.oz = oz;
			n.y = ( int )floorf( y + 0.5f );
			for( int d = 0; d < 4; d++ ) n.aEdge[d] = -1;
			aNodes.push_back( n );
			int idx = ( int )aNodes.size( ) - 1;
			mapCells[Key( ix, iz )].push_back( idx );
			return idx;
		}

		bool Run( )
		{
			LTVector vMin, vMax;
			g_pLTServer->GetWorldBox( vMin, vMax );
			fOriginX = floorf( vMin.x / kCell ) * kCell;
			fOriginZ = floorf( vMin.z / kCell ) * kCell;
			nCellsX = ( int )(( vMax.x - fOriginX ) / kCell ) + 1;
			nCellsZ = ( int )(( vMax.z - fOriginZ ) / kCell ) + 1;

			std::deque<int> queue;

			// Seed from every start/spawn point.
			GameStartPoint::StartPointList const& lst = GameStartPoint::GetStartPointList( );
			for( GameStartPoint::StartPointList::const_iterator it = lst.begin( ); it != lst.end( ); ++it )
			{
				if( !( *it ) || !( *it )->m_hObject ) continue;
				LTVector vPos;
				g_pLTServer->GetObjectPos(( *it )->m_hObject, &vPos );
				int ix = ( int )floorf(( vPos.x - fOriginX ) / kCell );
				int iz = ( int )floorf(( vPos.z - fOriginZ ) / kCell );
				float yFloor;
				if( !ProbeFloor( vPos.x, vPos.z, vPos.y + 16.0f, vPos.y - 256.0f, &yFloor ))
					continue;
				// Prefer the spawn point's own spot, then other points in the cell.
				int sox = ( int )( vPos.x - CenterX( ix )), soz = ( int )( vPos.z - CenterZ( iz ));
				if( sox > 24 ) sox = 24; if( sox < -24 ) sox = -24;
				if( soz > 24 ) soz = 24; if( soz < -24 ) soz = -24;
				float y = 0.0f;
				int ox = sox, oz = soz;
				bool bOk = CellOk( ix, iz, yFloor, &y, ox, oz );
				for( int k = 0; k < 13 && !bOk; k++ )
				{
					ox = s_aSample[k][0];
					oz = s_aSample[k][1];
					bOk = CellOk( ix, iz, yFloor, &y, ox, oz );
				}
				if( !bOk )
					continue;
				if( FindNode( ix, iz, y ) >= 0 )
					continue;
				queue.push_back( AddNode( ix, iz, y, ox, oz ));
			}

			if( aNodes.empty( ))
			{
				g_pLTServer->CPrint( "BotNav: no usable start points to seed from." );
				return false;
			}

			// Flood fill.
			while( !queue.empty( ))
			{
				int iCur = queue.front( );
				queue.pop_front( );

				for( int d = 0; d < 4; d++ )
				{
					if( aNodes[iCur].aEdge[d] >= 0 ) continue;

					int ix = aNodes[iCur].ix, iz = aNodes[iCur].iz;
					float yCur = ( float )aNodes[iCur].y;
					float fCurX = NodeX( aNodes[iCur] ), fCurZ = NodeZ( aNodes[iCur] );
					int nx = ix + s_aDirX[d], nz = iz + s_aDirZ[d];

					// Try the neighbour cell's centre first, then other spots in it,
					// until one is standable and walkable from here.
					float y = 0.0f;
					int ox = 0, oz = 0;
					int iNext = -1;
					bool bFound = false;
					for( int k = 0; k < 13 && !bFound; k++ )
					{
						int tox = s_aSample[k][0], toz = s_aSample[k][1];
						float ty;
						if( !CellOk( nx, nz, yCur, &ty, tox, toz ))
							continue;
						int iExist = FindNode( nx, nz, ty );
						float fBX, fBZ, fBY;
						if( iExist >= 0 )
						{
							fBX = NodeX( aNodes[iExist] );
							fBZ = NodeZ( aNodes[iExist] );
							fBY = ( float )aNodes[iExist].y;
						}
						else
						{
							fBX = CenterX( nx ) + ( float )tox;
							fBZ = CenterZ( nz ) + ( float )toz;
							fBY = ty;
						}
						if( !EdgeOk( fCurX, fCurZ, yCur, fBX, fBZ, fBY ))
						{
							if( iExist >= 0 ) break;	// The existing node is where it is.
							continue;
						}
						y = ty; ox = tox; oz = toz; iNext = iExist;
						bFound = true;
					}
					if( !bFound )
						continue;

					if( iNext < 0 )
					{
						if( aNodes.size( ) >= kMaxNodes )
						{
							g_pLTServer->CPrint( "BotNav: hit the node limit (%u); map partly covered.", kMaxNodes );
							queue.clear( );
							break;
						}
						iNext = AddNode( nx, nz, y, ox, oz );
						queue.push_back( iNext );
					}

					aNodes[iCur].aEdge[d] = iNext;
					aNodes[iNext].aEdge[s_aOpposite[d]] = iCur;
				}
			}

			return true;
		}

		// One box per node.  Where a neighbouring cell's volume would touch this
		// one but the two aren't walkably connected, pull this side in by 2 units
		// so the AI system doesn't treat them as neighbours.
		void BuildBoxes( std::vector<Box>& aBoxes ) const
		{
			aBoxes.resize( aNodes.size( ));
			for( size_t i = 0; i < aNodes.size( ); i++ )
			{
				const Node& n = aNodes[i];
				int x0 = ( int )CenterX( n.ix ) - kHalf, x1 = ( int )CenterX( n.ix ) + kHalf;
				int z0 = ( int )CenterZ( n.iz ) - kHalf, z1 = ( int )CenterZ( n.iz ) + kHalf;
				int y0 = n.y - kSlabBelow, y1 = n.y + kSlabAbove;

				for( int d = 0; d < 4; d++ )
				{
					if( n.aEdge[d] >= 0 ) continue;

					CellMap::const_iterator it = mapCells.find( Key( n.ix + s_aDirX[d], n.iz + s_aDirZ[d] ));
					if( it == mapCells.end( )) continue;

					bool bTouches = false;
					for( size_t k = 0; k < it->second.size( ); k++ )
					{
						const Node& m = aNodes[it->second[k]];
						if( m.y - kSlabBelow <= y1 && m.y + kSlabAbove >= y0 )
						{
							bTouches = true;
							break;
						}
					}
					if( !bTouches ) continue;

					switch( d )
					{
						case 0: x1 -= 2; break;
						case 1: x0 += 2; break;
						case 2: z1 -= 2; break;
						case 3: z0 += 2; break;
					}
				}

				aBoxes[i].vCenter = LTVector(( float )(( x0 + x1 ) / 2 ), ( float )(( y0 + y1 ) / 2 ), ( float )(( z0 + z1 ) / 2 ));
				aBoxes[i].vDims = LTVector(( float )(( x1 - x0 ) / 2 ), ( float )(( y1 - y0 ) / 2 ), ( float )(( z1 - z0 ) / 2 ));
			}
		}
	};

	// ----------------------------------------------------------------- //
	// Volume creation / cache.
	// ----------------------------------------------------------------- //

	bool CreateVolumes( const std::vector<Box>& aBoxes )
	{
		HCLASS hClass = g_pLTServer->GetClass( "AIVolume" );
		if( !hClass )
		{
			g_pLTServer->CPrint( "BotNav: AIVolume class not found." );
			return false;
		}

		CLiteObjectMgr* pLiteMgr = g_pGameServerShell->GetLiteObjectMgr( );
		uint32 nCreated = 0;
		for( size_t i = 0; i < aBoxes.size( ); i++ )
		{
			AIVolume* pVol = ( AIVolume* )g_pLTServer->CreateObject( hClass, LTNULL );
			if( !pVol )
				break;

			char szName[32];
			sprintf( szName, "BotNav%05u", ( uint32 )i );
			pVol->SetClass( hClass );
			pVol->SetName( szName );
			pVol->SetGeometry( aBoxes[i].vCenter, aBoxes[i].vDims );
			pLiteMgr->AddObject( pVol );
			pVol->SetRegistered( true );
			nCreated++;
		}

		// Rebuild the volume list and neighbour links.  Volumes that already
		// existed get their links/doors cleared first so Init doesn't double them.
		{
			CAISpatialRepresentationMgr::_listVolume* pList = g_pAIVolumeMgr->GetContainer( );
			for( size_t i = 0; i < pList->size( ); i++ )
			{
				AIVolume* pOld = ( AIVolume* )( *pList )[i];
				if( !pOld ) continue;
				pOld->ResetNeighbors( );
				pOld->ResetDoors( );
			}
		}
		g_pAIVolumeMgr->Init( );

		g_pLTServer->CPrint( "BotNav: created %u volumes (AI system now has %u).", nCreated, g_pAIVolumeMgr->GetNumVolumes( ));
		return nCreated > 0;
	}

	std::string CachePath( )
	{
		const char* szWorld = g_pServerMissionMgr ? g_pServerMissionMgr->GetCurrentWorldName( ) : "";
		std::string sName = ( szWorld && szWorld[0] ) ? szWorld : "unknown";
		for( size_t i = 0; i < sName.size( ); i++ )
		{
			char c = sName[i];
			if( c == '\\' || c == '/' || c == ':' || c == ' ' || c == '.' )
				sName[i] = '_';
		}
		return std::string( "BotNav\\" ) + sName + ".nav";
	}

	// The current map's walk graph (also used by the bots for path finding).
	Generator	s_Graph;
	bool		s_bGraphValid = false;
	std::string	s_sGraphWorld;

	bool SaveCache( const Generator& gen )
	{
		CreateDirectoryA( "BotNav", NULL );
		std::string sPath = CachePath( );
		FILE* fp = fopen( sPath.c_str( ), "wt" );
		if( !fp )
		{
			g_pLTServer->CPrint( "BotNav: could not write %s", sPath.c_str( ));
			return false;
		}
		fprintf( fp, "BOTNAV %d %u %d %d %d %d\n", kFileVersion, ( uint32 )gen.aNodes.size( ),
			( int )gen.fOriginX, ( int )gen.fOriginZ, gen.nCellsX, gen.nCellsZ );
		for( size_t i = 0; i < gen.aNodes.size( ); i++ )
		{
			const Node& n = gen.aNodes[i];
			fprintf( fp, "%d %d %d %d %d %d %d %d %d\n", n.ix, n.iz, n.y, n.aEdge[0], n.aEdge[1], n.aEdge[2], n.aEdge[3], n.ox, n.oz );
		}
		fclose( fp );
		g_pLTServer->CPrint( "BotNav: saved %s", sPath.c_str( ));
		return true;
	}

	bool LoadCache( Generator& gen )
	{
		std::string sPath = CachePath( );
		FILE* fp = fopen( sPath.c_str( ), "rt" );
		if( !fp )
			return false;

		int nVersion = 0, nOx = 0, nOz = 0, nCx = 0, nCz = 0;
		uint32 nCount = 0;
		bool bOk = ( fscanf( fp, "BOTNAV %d %u %d %d %d %d", &nVersion, &nCount, &nOx, &nOz, &nCx, &nCz ) == 6 ) &&
			nVersion == kFileVersion && nCount > 0 && nCount <= kMaxNodes;
		if( bOk )
		{
			gen.aNodes.clear( );
			gen.mapCells.clear( );
			gen.fOriginX = ( float )nOx;
			gen.fOriginZ = ( float )nOz;
			gen.nCellsX = nCx;
			gen.nCellsZ = nCz;
			for( uint32 i = 0; i < nCount && bOk; i++ )
			{
				int a[9];
				if( fscanf( fp, "%d %d %d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &a[8] ) != 9 )
				{
					bOk = false;
					break;
				}
				int idx = gen.AddNode( a[0], a[1], ( float )a[2], a[7], a[8] );
				for( int d = 0; d < 4; d++ )
					gen.aNodes[idx].aEdge[d] = ( a[3 + d] >= 0 && a[3 + d] < ( int )nCount ) ? a[3 + d] : -1;
			}
		}
		fclose( fp );

		if( !bOk )
			g_pLTServer->CPrint( "BotNav: %s is old or invalid; it will be regenerated.", sPath.c_str( ));
		else
			g_pLTServer->CPrint( "BotNav: loaded %u cells from %s", nCount, sPath.c_str( ));
		return bOk;
	}

	void SetGraph( const Generator& gen )
	{
		s_Graph = gen;
		s_bGraphValid = !gen.aNodes.empty( );
		const char* szWorld = g_pServerMissionMgr ? g_pServerMissionMgr->GetCurrentWorldName( ) : "";
		s_sGraphWorld = szWorld ? szWorld : "";
	}
}

static bool HasBotNavVolumes( )
{
	CLiteObjectMgr* pLiteMgr = g_pGameServerShell->GetLiteObjectMgr( );
	return pLiteMgr && pLiteMgr->FindObjectByName( "BotNav00000" ) != LTNULL;
}

uint32 BotNav::GetNumVolumes( )
{
	return g_pAIVolumeMgr ? g_pAIVolumeMgr->GetNumVolumes( ) : 0;
}

bool BotNav::Generate( bool bSave )
{
	if( !g_pAIVolumeMgr )
		return false;

	if( HasBotNavVolumes( ))
	{
		g_pLTServer->CPrint( "BotNav: bot navigation is already loaded. Use bot-navdelete and restart the level to generate again." );
		return false;
	}

	uint32 nStart = GetTickCount( );
	Generator gen;
	if( !gen.Run( ))
		return false;

	std::vector<Box> aBoxes;
	gen.BuildBoxes( aBoxes );
	g_pLTServer->CPrint( "BotNav: %u walkable cells found in %u ms.", ( uint32 )aBoxes.size( ), GetTickCount( ) - nStart );

	if( bSave )
		SaveCache( gen );

	SetGraph( gen );
	return CreateVolumes( aBoxes );
}

bool BotNav::EnsureNav( )
{
	if( !g_pAIVolumeMgr )
		return false;

	// Maps can contain a few hand-placed volumes (DE_RUINS has 12); those are not
	// enough for bots, so always add our own unless they are already loaded.
	if( HasBotNavVolumes( ) && HasGraph( ))
		return true;

	Generator gen;
	if( LoadCache( gen ))
	{
		SetGraph( gen );
		if( HasBotNavVolumes( ))
			return true;
		std::vector<Box> aBoxes;
		gen.BuildBoxes( aBoxes );
		return CreateVolumes( aBoxes );
	}

	g_pLTServer->CPrint( "BotNav: no navigation for this map yet - generating..." );
	return Generate( true );
}

// ----------------------------------------------------------------------- //
// Walk graph queries for the bots.
// ----------------------------------------------------------------------- //

bool BotNav::HasGraph( )
{
	if( !s_bGraphValid )
		return false;
	const char* szWorld = g_pServerMissionMgr ? g_pServerMissionMgr->GetCurrentWorldName( ) : "";
	return s_sGraphWorld == ( szWorld ? szWorld : "" );
}

uint32 BotNav::GetNumNodes( )
{
	return HasGraph( ) ? ( uint32 )s_Graph.aNodes.size( ) : 0;
}

LTVector BotNav::GetNodePos( int iNode )
{
	if( !HasGraph( ) || iNode < 0 || iNode >= ( int )s_Graph.aNodes.size( ))
		return LTVector( 0.0f, 0.0f, 0.0f );
	const Node& n = s_Graph.aNodes[iNode];
	return LTVector( s_Graph.NodeX( n ), ( float )n.y, s_Graph.NodeZ( n ));
}

int BotNav::GetNodeLink( int iNode, int nDir )
{
	if( !HasGraph( ) || iNode < 0 || iNode >= ( int )s_Graph.aNodes.size( ) || nDir < 0 || nDir > 3 )
		return -1;
	return s_Graph.aNodes[iNode].aEdge[nDir];
}

int BotNav::RandomNode( )
{
	uint32 n = GetNumNodes( );
	return n ? GetRandom( 0, ( int )n - 1 ) : -1;
}

int BotNav::FindNearestNode( const LTVector& vFeet, float fMaxDist )
{
	if( !HasGraph( ))
		return -1;

	int ix = ( int )floorf(( vFeet.x - s_Graph.fOriginX ) / kCell );
	int iz = ( int )floorf(( vFeet.z - s_Graph.fOriginZ ) / kCell );
	int nRadius = ( int )( fMaxDist / kCell ) + 1;

	int iBest = -1;
	float fBest = fMaxDist * fMaxDist;
	for( int r = 0; r <= nRadius; r++ )
	{
		for( int dx = -r; dx <= r; dx++ )
		{
			for( int dz = -r; dz <= r; dz++ )
			{
				if( abs( dx ) != r && abs( dz ) != r ) continue;	// ring only
				CellMap::const_iterator it = s_Graph.mapCells.find( Generator::Key( ix + dx, iz + dz ));
				if( it == s_Graph.mapCells.end( )) continue;
				for( size_t k = 0; k < it->second.size( ); k++ )
				{
					LTVector vNode = GetNodePos( it->second[k] );
					float fDY = vNode.y - vFeet.y;
					// Height difference counts extra so we pick the right floor.
					float fDist = ( vNode.x - vFeet.x ) * ( vNode.x - vFeet.x ) + ( vNode.z - vFeet.z ) * ( vNode.z - vFeet.z ) + 4.0f * fDY * fDY;
					if( fDist < fBest )
					{
						fBest = fDist;
						iBest = it->second[k];
					}
				}
			}
		}
		// Stop once something was found and the next ring can only be further.
		if( iBest >= 0 && ( float )( r * kCell ) * ( r * kCell ) > fBest )
			break;
	}
	return iBest;
}

namespace
{
	struct BlockedLink { int iA; int iB; float fUntil; };
	std::vector<BlockedLink> s_aBlocked;

	bool IsLinkBlocked( int iA, int iB )
	{
		if( s_aBlocked.empty( )) return false;
		float fNow = g_pLTServer->GetTime( );
		for( size_t i = 0; i < s_aBlocked.size( ); i++ )
		{
			const BlockedLink& b = s_aBlocked[i];
			// Entries from before a level change (time restarts) are ignored.
			if( b.fUntil <= fNow || b.fUntil > fNow + 301.0f ) continue;
			if(( b.iA == iA && b.iB == iB ) || ( b.iA == iB && b.iB == iA )) return true;
		}
		return false;
	}
}

namespace
{
	std::vector<BlockedLink> s_aStrikes;
}

void BotNav::BlockLink( int iA, int iB )
{
	// Two failures within a minute before a link is blocked, so one bot that was
	// merely jostled doesn't cut the graph.
	{
		float fT = g_pLTServer->GetTime( );
		bool bSecond = false;
		size_t iFree = s_aStrikes.size( );
		for( size_t i = 0; i < s_aStrikes.size( ); i++ )
		{
			BlockedLink& k = s_aStrikes[i];
			bool bLive = ( k.fUntil > fT && k.fUntil <= fT + 61.0f );
			if( !bLive ) { iFree = i; continue; }
			if(( k.iA == iA && k.iB == iB ) || ( k.iA == iB && k.iB == iA )) { bSecond = true; k.fUntil = 0.0f; break; }
		}
		if( !bSecond )
		{
			BlockedLink nk = { iA, iB, fT + 60.0f };
			if( iFree < s_aStrikes.size( )) s_aStrikes[iFree] = nk; else s_aStrikes.push_back( nk );
			return;
		}
	}
	float fNow = g_pLTServer->GetTime( );
	for( size_t i = 0; i < s_aBlocked.size( ); i++ )
	{
		BlockedLink& b = s_aBlocked[i];
		bool bSame = ( b.iA == iA && b.iB == iB ) || ( b.iA == iB && b.iB == iA );
		if( bSame || b.fUntil <= fNow || b.fUntil > fNow + 301.0f )
		{
			b.iA = iA; b.iB = iB; b.fUntil = fNow + 300.0f;
			return;
		}
	}
	BlockedLink nb = { iA, iB, fNow + 300.0f };
	s_aBlocked.push_back( nb );
}

int BotNav::FindVisibleNode( const LTVector& vFeet, float fMaxDist, int nSkip )
{
	if( !HasGraph( ))
		return -1;
	int ix = ( int )floorf(( vFeet.x - s_Graph.fOriginX ) / kCell );
	int iz = ( int )floorf(( vFeet.z - s_Graph.fOriginZ ) / kCell );
	int nRadius = ( int )( fMaxDist / kCell ) + 1;
	std::vector< std::pair< float, int > > aCand;
	for( int dx = -nRadius; dx <= nRadius; dx++ )
	{
		for( int dz = -nRadius; dz <= nRadius; dz++ )
		{
			CellMap::const_iterator it = s_Graph.mapCells.find( Generator::Key( ix + dx, iz + dz ));
			if( it == s_Graph.mapCells.end( )) continue;
			for( size_t k = 0; k < it->second.size( ); k++ )
			{
				LTVector vNode = GetNodePos( it->second[k] );
				float fDY = vNode.y - vFeet.y;
				if( fabsf( fDY ) > 150.0f ) continue;
				float fD = ( vNode.x - vFeet.x ) * ( vNode.x - vFeet.x ) + ( vNode.z - vFeet.z ) * ( vNode.z - vFeet.z ) + 4.0f * fDY * fDY;
				if( fD <= fMaxDist * fMaxDist )
					aCand.push_back( std::pair< float, int >( fD, it->second[k] ));
			}
		}
	}
	std::sort( aCand.begin( ), aCand.end( ));
	int nTests = 0, nSeen = 0;
	for( size_t i = 0; i < aCand.size( ) && nTests < 60; i++, nTests++ )
	{
		LTVector vNode = GetNodePos( aCand[i].second );
		if( !RayP( vFeet + LTVector( 0.0f, 24.0f, 0.0f ), vNode + LTVector( 0.0f, 24.0f, 0.0f ), LTNULL ))
		{
			if( nSeen++ == nSkip )
				return aCand[i].second;
		}
	}
	return ( nSkip == 0 ) ? FindNearestNode( vFeet, fMaxDist ) : -1;
}

int BotNav::FindNodeNearHoriz( const LTVector& vFeet, float fMaxDist, int nSkip, bool bBelowOnly )
{
	if( !HasGraph( ))
		return -1;
	int ix = ( int )floorf(( vFeet.x - s_Graph.fOriginX ) / kCell );
	int iz = ( int )floorf(( vFeet.z - s_Graph.fOriginZ ) / kCell );
	int nRadius = ( int )( fMaxDist / kCell ) + 1;
	std::vector< std::pair< float, int > > aCand;
	for( int dx = -nRadius; dx <= nRadius; dx++ )
	{
		for( int dz = -nRadius; dz <= nRadius; dz++ )
		{
			CellMap::const_iterator it = s_Graph.mapCells.find( Generator::Key( ix + dx, iz + dz ));
			if( it == s_Graph.mapCells.end( )) continue;
			for( size_t k = 0; k < it->second.size( ); k++ )
			{
				LTVector vNode = GetNodePos( it->second[k] );
				if( bBelowOnly && vNode.y > vFeet.y - 40.0f ) continue;
				float fD = ( vNode.x - vFeet.x ) * ( vNode.x - vFeet.x ) + ( vNode.z - vFeet.z ) * ( vNode.z - vFeet.z );
				if( fD <= fMaxDist * fMaxDist )
					aCand.push_back( std::pair< float, int >( fD, it->second[k] ));
			}
		}
	}
	std::sort( aCand.begin( ), aCand.end( ));
	return ( nSkip >= 0 && nSkip < ( int )aCand.size( )) ? aCand[nSkip].second : -1;
}

namespace
{
	std::vector<BotNav::LadderInfo> s_aLadders;
}

void BotNav::BuildLadders( )
{
	s_aLadders.clear( );
	if( !HasGraph( ) || !g_pLTServer ) return;
	for( int nPass = 0; nPass < 2; nPass++ )
	{
		HOBJECT hObj = ( nPass == 0 ) ? g_pLTServer->GetNextObject( LTNULL ) : g_pLTServer->GetNextInactiveObject( LTNULL );
		while( hObj )
		{
			if( IsKindOf( hObj, "Ladder" ))
			{
				LTVector vPos, vDims;
				g_pLTServer->GetObjectPos( hObj, &vPos );
				g_pPhysicsLT->GetObjectDims( hObj, &vDims );
				float fBottom = vPos.y - vDims.y;
				float fTop = vPos.y + vDims.y;
				int iB = FindNodeNearHoriz( LTVector( vPos.x, fBottom + 20.0f, vPos.z ), 192.0f, 0, false );
				// Pick the closest nodes to the foot and to the head of the ladder.
				int iBot = -1, iTop = -1;
				float fBotD = 1e30f, fTopD = 1e30f;
				for( int nSkip = 0; nSkip < 90; nSkip++ )
				{
					int iC = FindNodeNearHoriz( vPos, 224.0f, nSkip, false );
					if( iC < 0 ) break;
					LTVector vN = GetNodePos( iC );
					float fH = ( vN.x - vPos.x ) * ( vN.x - vPos.x ) + ( vN.z - vPos.z ) * ( vN.z - vPos.z );
					float fDb = ( vN.y >= fBottom - 60.0f && vN.y <= fBottom + 140.0f ) ? fH : 1e30f;
					float fDt = ( vN.y >= fTop - 260.0f && vN.y <= fTop + 30.0f ) ? fH : 1e30f;
					if( fDb < fBotD ) { fBotD = fDb; iBot = iC; }
					if( fDt < fTopD ) { fTopD = fDt; iTop = iC; }
				}
				( void )iB;
				if( iBot >= 0 && iTop >= 0 && iBot != iTop && GetNodePos( iTop ).y - GetNodePos( iBot ).y > 60.0f )
				{
					LadderInfo li;
					li.vCenter = vPos;
					li.vHalf = vDims;
					// Find a vertical column (body width) free of solid geometry from the top floor
					// down to the bottom floor, as close to the ladder as possible.
					{
						float yHi = GetNodePos( iTop ).y + 90.0f;
						float yLo = GetNodePos( iBot ).y + 18.0f;
						static const float kOff[5][2] = { { 0.0f, 0.0f }, { 18.0f, 0.0f }, { -18.0f, 0.0f }, { 0.0f, 18.0f }, { 0.0f, -18.0f } };
						bool bFound = false;
						float fBestD = 1e30f;
						LTVector vBest( vPos.x, 0.0f, vPos.z );
						for( int gx = -4; gx <= 4; gx++ )
						{
							for( int gz = -4; gz <= 4; gz++ )
							{
								float fdx = gx * 24.0f, fdz = gz * 24.0f;
								float fD = fdx * fdx + fdz * fdz;
								if( fD >= fBestD ) continue;
								bool bClear = true;
								for( int k = 0; k < 5 && bClear; k++ )
								{
									float fx = vPos.x + fdx + kOff[k][0], fz = vPos.z + fdz + kOff[k][1];
									if( Ray( LTVector( fx, yHi, fz ), LTVector( fx, yLo, fz ), LTNULL )) bClear = false;
								}
								if( bClear )
								{
									fBestD = fD;
									vBest = LTVector( vPos.x + fdx, 0.0f, vPos.z + fdz );
									bFound = true;
								}
							}
						}
						li.vClimb = vBest;
						g_pLTServer->CPrint( "bots: ladder climb column <%.0f,%.0f> (%s)", vBest.x, vBest.z, bFound ? "clear" : "none clear, using centre" );
					}
					li.iBottom = iBot;
					li.iTop = iTop;
					s_aLadders.push_back( li );
					g_pLTServer->CPrint( "bots: ladder at <%.0f,%.0f,%.0f> (%.0f..%.0f) nodes %d -> %d", vPos.x, vPos.y, vPos.z, fBottom, fTop, iBot, iTop );
				}
				else
					g_pLTServer->CPrint( "bots: ladder at <%.0f,%.0f,%.0f> has no usable nodes (bottom %d top %d)", vPos.x, vPos.y, vPos.z, iBot, iTop );
			}
			hObj = ( nPass == 0 ) ? g_pLTServer->GetNextObject( hObj ) : g_pLTServer->GetNextInactiveObject( hObj );
		}
	}
}

uint32 BotNav::GetNumLadders( )
{
	return ( uint32 )s_aLadders.size( );
}

const BotNav::LadderInfo* BotNav::GetLadder( int i )
{
	return ( i >= 0 && i < ( int )s_aLadders.size( )) ? &s_aLadders[i] : LTNULL;
}

int BotNav::FindLadderLink( int iA, int iB, bool* pbUp )
{
	for( size_t i = 0; i < s_aLadders.size( ); i++ )
	{
		if( s_aLadders[i].iBottom == iA && s_aLadders[i].iTop == iB ) { if( pbUp ) *pbUp = true; return ( int )i; }
		if( s_aLadders[i].iTop == iA && s_aLadders[i].iBottom == iB ) { if( pbUp ) *pbUp = false; return ( int )i; }
	}
	return -1;
}

bool BotNav::FindPath( int iFrom, int iTo, std::vector<int>& aPath )
{
	aPath.clear( );
	if( !HasGraph( )) return false;
	int nNodes = ( int )s_Graph.aNodes.size( );
	if( iFrom < 0 || iTo < 0 || iFrom >= nNodes || iTo >= nNodes ) return false;
	if( iFrom == iTo ) { aPath.push_back( iTo ); return true; }

	static std::vector<float> s_aCost;
	static std::vector<int> s_aParent;
	static std::vector<uint32> s_aStamp;
	static uint32 s_nStamp = 0;
	if(( int )s_aCost.size( ) != nNodes )
	{
		s_aCost.assign( nNodes, 0.0f );
		s_aParent.assign( nNodes, -1 );
		s_aStamp.assign( nNodes, 0 );
	}
	if( ++s_nStamp == 0 ) { s_aStamp.assign( nNodes, 0 ); s_nStamp = 1; }

	LTVector vGoal = GetNodePos( iTo );
	typedef std::pair< float, int > Entry;
	std::priority_queue< Entry, std::vector<Entry>, std::greater<Entry> > open;

	s_aStamp[iFrom] = s_nStamp;
	s_aCost[iFrom] = 0.0f;
	s_aParent[iFrom] = -1;
	open.push( Entry( 0.0f, iFrom ));

	bool bFound = false;
	while( !open.empty( ))
	{
		Entry e = open.top( );
		open.pop( );
		int iCur = e.second;
		if( iCur == iTo ) { bFound = true; break; }

		LTVector vCur = GetNodePos( iCur );
		float fEst = sqrtf( vCur.DistSqr( vGoal ));
		if( e.first > s_aCost[iCur] + fEst + 0.01f ) continue;	// stale entry

		for( int d = 0; d < 4; d++ )
		{
			int iNext = s_Graph.aNodes[iCur].aEdge[d];
			if( iNext < 0 ) continue;
			if( IsLinkBlocked( iCur, iNext )) continue;
			LTVector vNext = GetNodePos( iNext );
			float fCost = s_aCost[iCur] + sqrtf( vCur.DistSqr( vNext ));
			if( s_aStamp[iNext] == s_nStamp && s_aCost[iNext] <= fCost ) continue;
			s_aStamp[iNext] = s_nStamp;
			s_aCost[iNext] = fCost;
			s_aParent[iNext] = iCur;
			open.push( Entry( fCost + sqrtf( vNext.DistSqr( vGoal )), iNext ));
		}

		// Ladders join two otherwise separate floors.
		for( size_t L = 0; L < s_aLadders.size( ); L++ )
		{
			int iNextL = -1;
			if( s_aLadders[L].iBottom == iCur ) iNextL = s_aLadders[L].iTop;
			else if( s_aLadders[L].iTop == iCur ) iNextL = s_aLadders[L].iBottom;
			if( iNextL < 0 || IsLinkBlocked( iCur, iNextL )) continue;
			LTVector vNextL = GetNodePos( iNextL );
			float fCostL = s_aCost[iCur] + sqrtf( vCur.DistSqr( vNextL )) * 1.5f + 150.0f;
			if( s_aStamp[iNextL] == s_nStamp && s_aCost[iNextL] <= fCostL ) continue;
			s_aStamp[iNextL] = s_nStamp;
			s_aCost[iNextL] = fCostL;
			s_aParent[iNextL] = iCur;
			open.push( Entry( fCostL + sqrtf( vNextL.DistSqr( vGoal )), iNextL ));
		}
	}

	if( !bFound ) return false;

	for( int i = iTo; i >= 0; i = s_aParent[i] )
	{
		aPath.push_back( i );
		if( i == iFrom ) break;
	}
	std::reverse( aPath.begin( ), aPath.end( ));
	return true;
}

bool BotNav::DeleteCache( )
{
	std::string sPath = CachePath( );
	bool bOk = ( remove( sPath.c_str( )) == 0 );
	g_pLTServer->CPrint( bOk ? "BotNav: deleted %s" : "BotNav: no cache file %s", sPath.c_str( ));
	return bOk;
}

// Draws the floor outline of every volume within fRadius of vCenter, plus a
// line across each walkable connection.  Bot volumes are green, the map's own
// volumes yellow.  Lines are streamed to clients by the debug line system.
void BotNav::Show( bool bShow, const LTVector& vCenter )
{
	DebugLineSystem& system = LineSystem::GetSystem( "BotNavView" );
	system.Clear( );
	if( !bShow || !g_pAIVolumeMgr )
	{
		g_pLTServer->CPrint( "BotNav: view cleared." );
		return;
	}

	const float fRadius = 1500.0f;
	const uint32 kMaxLines = 12000;
	system.SetMaxLines( kMaxLines );

	uint32 nLines = 0, nShown = 0;
	CAISpatialRepresentationMgr::_listVolume* pList = g_pAIVolumeMgr->GetContainer( );
	for( size_t i = 0; i < pList->size( ) && nLines + 8 < kMaxLines; i++ )
	{
		AISpatialRepresentation* pVol = ( *pList )[i];
		if( !pVol ) continue;
		const LTVector& c = pVol->GetCenter( );
		const LTVector& d = pVol->GetDims( );
		float dx = c.x - vCenter.x, dz = c.z - vCenter.z;
		if( dx * dx + dz * dz > fRadius * fRadius ) continue;

		bool bOurs = ( strncmp( pVol->GetName( ), "BotNav", 6 ) == 0 );
		DebugLine::Color col = bOurs ? Color::Green : Color::Yellow;
		float y = bOurs ? ( c.y - d.y + ( float )kSlabBelow + 2.0f ) : ( c.y - d.y + 2.0f );
		LTVector p0( c.x - d.x, y, c.z - d.z ), p1( c.x + d.x, y, c.z - d.z );
		LTVector p2( c.x + d.x, y, c.z + d.z ), p3( c.x - d.x, y, c.z + d.z );
		system.AddLine( p0, p1, col );
		system.AddLine( p1, p2, col );
		system.AddLine( p2, p3, col );
		system.AddLine( p3, p0, col );
		nLines += 4;
		nShown++;
	}

	g_pLTServer->CPrint( "BotNav: drawing %u volumes within %.0f units (%u lines). bot-navhide to clear.", nShown, fRadius, nLines );
}

void BotNav::PrintInfo( )
{
	g_pLTServer->CPrint( "BotNav: %u AI volumes on this map (bot navigation %s). Cache file: %s", GetNumVolumes( ),
		HasBotNavVolumes( ) ? "loaded" : "NOT loaded", CachePath( ).c_str( ));
}
