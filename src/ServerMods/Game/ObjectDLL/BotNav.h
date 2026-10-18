// ----------------------------------------------------------------------- //
//
// MODULE  : BotNav.h
//
// PURPOSE : Automatic navigation (AIVolume) generation for multiplayer bots,
//           in the spirit of Quake 3's AAS.  Walkable floor is sampled on a
//           grid by flood-filling from the level's start points, and one
//           AIVolume is created per walkable cell.  Results are cached per map
//           in BotNav\<map>.nav in the game folder.
//
// ----------------------------------------------------------------------- //

#ifndef __BOT_NAV_H__
#define __BOT_NAV_H__

#include <vector>

namespace BotNav
{
	// Number of AIVolumes the AI system currently knows about.
	uint32 GetNumVolumes( );

	// Make sure the current map has navigation: load the cache, or generate
	// (and save) it if there is none.  Returns false if no nav could be made.
	bool EnsureNav( );

	// Throw away the cache file for this map and generate again.
	// Only works on a map that has no navigation loaded yet (otherwise the
	// level must be restarted first).
	bool Generate( bool bSave );

	// Delete the cache file for the current map.
	bool DeleteCache( );

	// Debug drawing of the volumes near vCenter.
	void Show( bool bShow, const LTVector& vCenter );

	// Prints counts to the console.
	void PrintInfo( );

	// ---- Walk graph for bots (valid once EnsureNav succeeded on this map) ----
	// Nodes are 64-unit floor cells; positions are on the floor at the cell centre.
	bool		HasGraph( );
	uint32		GetNumNodes( );
	LTVector	GetNodePos( int iNode );
	int			GetNodeLink( int iNode, int nDir );		// nDir 0..3, -1 if none
	int			RandomNode( );
	// Nearest node to a floor position (height counts extra).  -1 if none within fMaxDist.
	int			FindNearestNode( const LTVector& vFeet, float fMaxDist = 256.0f );
	// Like FindNearestNode, but only nodes with a clear line from vFeet (not behind a wall).
	int			FindVisibleNode( const LTVector& vFeet, float fMaxDist, int nSkip = 0 );
	// nSkip-th nearest node by horizontal distance only (optionally only nodes well below vFeet).
	int			FindNodeNearHoriz( const LTVector& vFeet, float fMaxDist, int nSkip, bool bBelowOnly );
	// A* over the walk graph.  aPath includes both ends.
	bool		FindPath( int iFrom, int iTo, std::vector<int>& aPath );

	// ---- Ladders ----
	struct LadderInfo
	{
		LTVector	vCenter;	// Ladder volume centre.
		LTVector	vHalf;		// Ladder volume half extents.
		LTVector	vClimb;		// x/z of a clear vertical column next to the ladder to climb in.
		int			iBottom;	// Walk node at the foot of the ladder.
		int			iTop;		// Walk node at the top.
	};
	// Finds the level's Ladder volumes and links their ends in the walk graph.
	void		BuildLadders( );
	uint32		GetNumLadders( );
	const LadderInfo*	GetLadder( int i );
	// If iA -> iB is a ladder link, returns the ladder index (and whether it goes up), else -1.
	int			FindLadderLink( int iA, int iB, bool* pbUp );
	// Mark the link between two neighbouring nodes as impassable for a while
	// (a bot failed to cross it).  A* avoids blocked links.
	void		BlockLink( int iA, int iB );
}

#endif // __BOT_NAV_H__
