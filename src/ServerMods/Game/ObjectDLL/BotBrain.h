// ----------------------------------------------------------------------- //
//
// MODULE  : BotBrain.h
//
// PURPOSE : Decision making for multiplayer bots.  The brain only "sees"
//           what a player could see, reacts with a human-ish delay, turns
//           and aims at a limited speed, and drives the bot through the same
//           inputs a player has (move direction, facing, jump, fire).
//
// ----------------------------------------------------------------------- //

#ifndef __BOT_BRAIN_H__
#define __BOT_BRAIN_H__

#include <vector>

class CPlayerObj;

class CBotBrain
{
	public :

		// nSkill: 0 = easy ... 3 = hard.
		CBotBrain( int nSkill );
		virtual ~CBotBrain( );

		void	Update( CPlayerObj* pBot, float fFrameTime );
		void	OnRespawn( CPlayerObj* pBot );
		void	OnDeath( CPlayerObj* pBot );

		// Gives this bot a personality (picked from its name, so "Vixen" always
		// plays the same way).  Returns the style's name for the console.
		const char* SetPersonality( const char* szName );

		// Gunfire and the like that bots within earshot can hear.
		static void ReportNoise( HOBJECT hSource, const LTVector& vPos, float fRadius );

		// Adaptive difficulty: -1 .. +1 skill levels on top of the chosen one.
		static void  SetAdaptiveSkill( float f );
		static float GetAdaptiveSkill( );
		static void  SetChatEnabled( bool b );

		// Says hello when the bot joins.
		void	Greet( CPlayerObj* pBot );
		void	DoGreet( CPlayerObj* pBot );

		int		GetSkill( ) const { return m_nSkill; }
		float	GetDamageScale( ) const;		// Multiplier on damage this bot deals.
		float	GetFireDelayScale( ) const;	// Multiplier on time between shots.
		float	GetPerturbScale( ) const;		// Multiplier on weapon spread.

	private :

		// Senses / targeting
		HOBJECT	FindVisibleEnemy( CPlayerObj* pBot, const LTVector& vEye, float fYaw );
		bool	CanSee( const LTVector& vEye, HOBJECT hTarget, LTVector* pvAimPoint );
		bool	IsEnemy( CPlayerObj* pBot, HOBJECT hOther );

		// Movement
		bool	PlanPathTo( const LTVector& vFeet, const LTVector& vGoalFeet );
		bool	PlanPathToNode( const LTVector& vFeet, int iGoal );
		bool	FollowPath( const LTVector& vFeet, LTVector* pvMoveDir );
		void	PickRoamGoal( const LTVector& vFeet );
		bool	PickPlayerObjective( CPlayerObj* pBot, const LTVector& vFeet );
		void	AvoidWalls( const LTVector& vFeet, LTVector* pvMoveDir );

		// Weapons
		void	ChooseWeapon( CPlayerObj* pBot, float fTargetDist );

		// Pickups
		void	TryTouchNearbyItems( CPlayerObj* pBot, const LTVector& vPos );
		void	SeekItems( CPlayerObj* pBot, const LTVector& vFeet, bool bHealthOnly = false );
		bool	HearNoise( CPlayerObj* pBot, LTVector* pvPos );
		bool	IsItemIgnored( HOBJECT hItem );
		void	RememberBadSpot( const LTVector& vPos );
		bool	RescueTeleport( CPlayerObj* pBot, const LTVector& vPos, float fMinDist );
		bool	IsBadSpot( const LTVector& vPos );
		void	IgnoreItem( HOBJECT hItem, float fSeconds );

		// Chat / taunts
		void	Say( CPlayerObj* pBot, const char* szMsg, bool bForce = false );
		void	OnFrag( CPlayerObj* pBot );

		// Aim helpers
		void	TurnTowards( float fWantYaw, float fWantPitch, float fFrameTime, float fTurnRate );

	private :

		int		m_nSkill;

		// Facing (what the "mouse" is doing).
		float	m_fYaw;
		float	m_fPitch;

		// Combat
		LTObjRef m_hTarget;
		float	m_fTargetSeenTime;		// When we first saw the current target.
		float	m_fTargetLastSeenTime;
		LTVector m_vTargetLastSeenPos;
		bool	m_bHaveLastSeenPos;
		LTVector m_vAimError;			// Current aim offset (world units at target).
		float	m_fNextAimErrorTime;
		float	m_fStrafeDir;
		float	m_fNextStrafeChange;
		float	m_fNextJumpTime;
		float	m_fNextWeaponCheck;
		float	m_fLastHealth;
		LTObjRef m_hLastAttacker;
		float	m_fLastAttackedTime;

		// Navigation
		std::vector<int> m_aPath;
		uint32	m_nPathIndex;
		int		m_iGoalNode;
		float	m_fNextRepathTime;
		LTVector m_vLastProgressPos;
		float	m_fLastProgressTime;
		float	m_fGoalTimeout;

		// Fire discipline (bursts) and unsticking
		float	m_fBurstEnd;
		float	m_fPauseEnd;
		float	m_fUnstuckUntil;
		LTVector m_vUnstuckDir;

		// Pickups / weapon tactics
		LTObjRef m_hItemGoal;
		LTVector m_vItemPos;
		float	m_fNextItemCheck;
		float	m_fItemGoalTimeout;
		bool	m_bMeleeMode;
		LTVector m_vSmoothMove;
		float	m_fNearItemSince;
		float	m_fNextRetarget;
		float	m_fNoPathSince;
		float	m_fStuckWindowStart;
		int		m_nStuckCount;
		LTObjRef m_hObjectivePlayer;	// Someone we've decided to go and find.
		float	m_fObjectiveReplan;
		float	m_fObjectiveTimeout;
		float	m_fNextLookTime;
		float	m_fLookYaw;
		float	m_fLeanUntil;
		float	m_fLeanSide;
		float	m_fNextStrafeProbe;
		LTVector m_vAimErrorGoal;

		// Personality
		float	m_fAccuracy;		// Multiplier on aim error (lower = better shot).
		float	m_fAggression;		// 0 = careful, 1 = reckless.
		float	m_fPrefDist;		// Distance it likes to fight at.
		float	m_fRetreatHealth;	// Health fraction at which it breaks off to heal.
		float	m_fHuntTime;		// How long it keeps hunting someone it lost.
		const char* m_szFavWeapon;

		// Retreat / investigate
		float	m_fDuckUntil;
		float	m_fGreetAt;
		int		m_nLastFrags;
		int		m_nStreak;
		float	m_fNextChat;
		int		m_nChatStyle;		// 0 = cocky, 1 = friendly, 2 = plain.
		LTObjRef m_hLastKiller;
		LTObjRef m_hLastEnemy;
		float	m_fNextDoorTime;
		std::vector<LTVector> m_aTrail;	// Where we have been (for walking back out).
		std::vector<LTVector> m_aBack;
		uint32	m_nBackIdx;
		float	m_fBackUntil;
		float	m_fBackLast;
		float	m_fBackProg;
		LTVector m_vBackLastPos;
		float	m_fWallUntil;	// Wall-following mode (trapped against walls).
		float	m_fWallNext;
		float	m_fWallProgT;
		float	m_fWallSide;
		LTVector m_vWallDir;
		LTVector m_vWallStart;
		LTVector m_vWallProgPos;
		HOBJECT	m_hLastDoor;
		int		m_nDoorTries;
		float	m_fLastDoorTry;
		float	m_fCombatPathUntil;
		LTVector	m_vDropGoal;
		float	m_fDropUntil;
		int	m_iLadder;
		int	m_nLadderPhase;
		float	m_fLadderStart;
		float	m_fLadderY;
		float	m_fLadderYTime;
		bool	m_bLadderUp;
		float	m_fNextDuckProbe;
		float	m_fNextInvestigate;
		float	m_fInvestigateArrive;
		float	m_fNextSkipCheck;	// Throttles for the expensive checks.
		float	m_fNextScan;
		float	m_fNextTouch;
		bool	m_bRetreating;
		float	m_fRetreatUntil;
		LTVector m_vInvestigatePos;
		float	m_fInvestigateUntil;
		struct BadSpot { LTVector vPos; float fUntil; };
		std::vector<BadSpot> m_aBadSpots;
		struct IgnoredItem { HOBJECT hItem; float fUntil; };
		std::vector<IgnoredItem> m_aIgnoredItems;
};

#endif // __BOT_BRAIN_H__
