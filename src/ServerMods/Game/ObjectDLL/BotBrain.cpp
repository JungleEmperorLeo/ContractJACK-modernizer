// ----------------------------------------------------------------------- //
//
// MODULE  : BotBrain.cpp
//
// PURPOSE : Decision making for multiplayer bots.  See BotBrain.h.
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "BotBrain.h"
#include "BotNav.h"
#include "PlayerObj.h"
#include "Destructible.h"
#include "Weapon.h"
#include "WeaponMgr.h"
#include "GameServerShell.h"
#include "ServerUtilities.h"
#include "CommonUtilities.h"
#include "PickupItem.h"
#include "WeaponItems.h"
#include "AmmoBox.h"
#include "GearItems.h"
#include "ModItem.h"
#include "TeamMgr.h"
#include "MsgIDs.h"
#include "Door.h"
#include <math.h>

namespace
{
	const float kPi = 3.14159265f;

	float Now( ) { return g_pLTServer->GetTime( ); }

	float WrapAngle( float a )
	{
		while( a > kPi ) a -= 2.0f * kPi;
		while( a < -kPi ) a += 2.0f * kPi;
		return a;
	}

	float RandF( float lo, float hi )
	{
		return lo + ( hi - lo ) * ( float )GetRandom( 0, 1000 ) / 1000.0f;
	}

	// Only closed-door candidates: lets a ray find a door through anything else.
	bool DoorOnlyFn( HOBJECT hObj, void* /*pUserData*/ )
	{
		return ( OT_WORLDMODEL == GetObjectType( hObj ) && IsKindOf( hObj, "Door" )) ? true : false;
	}

	// Anything solid except the object passed as user data (the bot itself).
	bool AnySolidButSelfFn( HOBJECT hObj, void* pUserData )
	{
		return hObj != ( HOBJECT )pUserData;
	}

	// Is there anything solid within fLen units of vFrom along vDir (not the bot itself)?
	bool WallProbe( const LTVector& vFrom, const LTVector& vDir, float fLen, HOBJECT hSelf )
	{
		IntersectQuery q;
		IntersectInfo i;
		q.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
		q.m_FilterFn = AnySolidButSelfFn;
		q.m_pUserData = ( void* )hSelf;
		q.m_From = vFrom;
		q.m_To = vFrom + vDir * fLen;
		return !!g_pLTServer->IntersectSegment( &q, &i );
	}

	// Turn a horizontal direction by fAng radians (positive = to the right).
	LTVector RotY( const LTVector& v, float fAng )
	{
		float c = cosf( fAng ), s = sinf( fAng );
		return LTVector( v.x * c + v.z * s, 0.0f, -v.x * s + v.z * c );
	}

	bool SightFilterFn( HOBJECT hObj, void* /*pUserData*/ )
	{
		return ( IsMainWorld( hObj ) || OT_WORLDMODEL == GetObjectType( hObj ));
	}

	// For seeing / shooting: world geometry AND solid props (crates, railings...)
	// block the view, but characters do not.
	bool ViewBlockerFn( HOBJECT hObj, void* /*pUserData*/ )
	{
		if( IsMainWorld( hObj ) || OT_WORLDMODEL == GetObjectType( hObj )) return true;
		if( OT_MODEL != GetObjectType( hObj )) return false;
		// Only real props: not pickups (solid, but you can see over/around them),
		// bodies, attachments or characters.
		return IsKindOf( hObj, "Prop" ) ? true : false;
	}

	// The engine's pitch sign convention, detected once: +1 if a positive
	// pitch makes Forward() point down.
	float PitchSign( )
	{
		static float s_fSign = 0.0f;
		if( s_fSign == 0.0f )
		{
			LTRotation r( 0.5f, 0.0f, 0.0f );
			s_fSign = ( r.Forward( ).y < 0.0f ) ? 1.0f : -1.0f;
		}
		return s_fSign;
	}

	// Yaw / pitch that look along vDir.
	void DirToAngles( const LTVector& vDir, float* pfYaw, float* pfPitch )
	{
		*pfYaw = atan2f( vDir.x, vDir.z );
		float fHoriz = sqrtf( vDir.x * vDir.x + vDir.z * vDir.z );
		*pfPitch = -atan2f( vDir.y, fHoriz ) * PitchSign( );
	}

	// Per-skill tuning.
	struct SkillData
	{
		float fReaction;		// Seconds before reacting to a newly seen enemy.
		float fTurnRate;		// Radians per second.
		float fAimError;		// Aim wobble, world units at 1000 units distance.
		float fFov;				// Half field of view (radians).
		float fFireCone;		// Fire when within this many units of the aim point (scaled by distance).
		float fBurstMin, fBurstMax;	// Seconds of continuous fire (0 = never pause).
		float fPauseMin, fPauseMax;	// Seconds between bursts.
		float fDamageScale;		// Damage dealt multiplier (UT-style handicap).
		float fFireDelayScale;	// Slower trigger finger.
		float fPerturbScale;		// Wider weapon spread.
	};
	const SkillData s_aSkill[5] =
	{
		{ 1.10f, 1.8f,  260.0f, 0.90f, 70.0f, 0.30f, 0.70f, 0.90f, 2.00f, 0.30f, 1.8f, 1.6f },
		{ 0.70f, 3.0f,  150.0f, 1.00f, 55.0f, 0.50f, 1.20f, 0.40f, 1.10f, 0.45f, 1.4f, 1.3f },
		{ 0.57f, 4.0f,  108.0f, 1.07f, 47.0f, 0.65f, 1.60f, 0.30f, 0.85f, 0.57f, 1.27f, 1.15f },
		{ 0.45f, 5.0f,   70.0f, 1.15f, 40.0f, 0.80f, 2.00f, 0.20f, 0.60f, 0.70f, 1.15f, 1.0f },
		{ 0.22f, 9.0f,   20.0f, 1.30f, 28.0f, 0.00f, 0.00f, 0.00f, 0.00f, 1.00f, 1.0f, 1.0f },
	};

	const SkillData& Skill( int n ) { return s_aSkill[( n < 2 ) ? 2 : ( n > 4 ) ? 4 : n]; }

	// Adaptive difficulty (set from BotMgr): -1 .. +1 levels, positive when the
	// human is winning and the bots should tighten up.
	float	s_fAdapt = 0.0f;
	bool	s_bChat = true;
	float	s_fLastChatAny = -100.0f;

	SkillData BlendedSkill( int nBase, float fAdapt )
	{
		float f = ( float )(( nBase < 2 ) ? 2 : ( nBase > 4 ) ? 4 : nBase ) + fAdapt;
		if( f < 2.0f ) f = 2.0f;
		if( f > 4.0f ) f = 4.0f;
		int lo = ( int )f;
		if( lo > 3 ) lo = 3;
		float t = f - ( float )lo;
		const SkillData& a = s_aSkill[lo];
		const SkillData& c = s_aSkill[lo + 1];
		SkillData r;
		r.fReaction = a.fReaction + ( c.fReaction - a.fReaction ) * t;
		r.fTurnRate = a.fTurnRate + ( c.fTurnRate - a.fTurnRate ) * t;
		r.fAimError = a.fAimError + ( c.fAimError - a.fAimError ) * t;
		r.fFov = a.fFov + ( c.fFov - a.fFov ) * t;
		r.fFireCone = a.fFireCone + ( c.fFireCone - a.fFireCone ) * t;
		r.fBurstMin = a.fBurstMin + ( c.fBurstMin - a.fBurstMin ) * t;
		r.fBurstMax = a.fBurstMax + ( c.fBurstMax - a.fBurstMax ) * t;
		r.fPauseMin = a.fPauseMin + ( c.fPauseMin - a.fPauseMin ) * t;
		r.fPauseMax = a.fPauseMax + ( c.fPauseMax - a.fPauseMax ) * t;
		r.fDamageScale = a.fDamageScale + ( c.fDamageScale - a.fDamageScale ) * t;
		r.fFireDelayScale = a.fFireDelayScale + ( c.fFireDelayScale - a.fFireDelayScale ) * t;
		r.fPerturbScale = a.fPerturbScale + ( c.fPerturbScale - a.fPerturbScale ) * t;
		return r;
	}

	const char* PickLine( const char* const* a, int n ) { return a[GetRandom( 0, n - 1 )]; }

	LTVector FeetOf( HOBJECT hObj )
	{
		LTVector vPos, vDims;
		g_pLTServer->GetObjectPos( hObj, &vPos );
		g_pPhysicsLT->GetObjectDims( hObj, &vDims );
		return vPos - LTVector( 0.0f, vDims.y, 0.0f );
	}
}

// ----------------------------------------------------------------------- //

namespace
{
	// What a player would do when trapped: /kill and respawn elsewhere.
	void KillSelf( CPlayerObj* pBot )
	{
		DamageStruct damage;
		damage.eType	= DT_UNSPECIFIED;
		damage.fDamage	= damage.kInfiniteDamage;
		damage.hDamager = NULL;
		damage.vDir.Init( 0, 1, 0 );
		damage.DoDamage( pBot, pBot->m_hObject );
	}

	// Targets on another floor (above/below, or at a steep angle) aren't engaged:
	// the bot can't reach them and shooting through gaps just wastes ammo.
	bool OnOtherFloor( const LTVector& vEye, const LTVector& vTarget )
	{
		float fDY = fabsf( vTarget.y - vEye.y );
		float fH = sqrtf(( vTarget.x - vEye.x ) * ( vTarget.x - vEye.x ) + ( vTarget.z - vEye.z ) * ( vTarget.z - vEye.z ));
		return fDY > 150.0f || fDY > fH * 0.7f + 40.0f;
	}
}

namespace
{
	struct Personality
	{
		const char* szStyle;
		float fAccuracy, fAggression, fPrefDist, fRetreatHealth, fHuntTime;
		const char* szFavWeapon;
	};
	const Personality s_aPersonalities[] =
	{
		//  style        aim   aggr  dist   retreat hunt  favourite
		{ "rusher",     1.20f, 0.95f, 180.0f, 0.15f, 14.0f, "Shotgun" },
		{ "marksman",   0.75f, 0.30f, 950.0f, 0.45f,  6.0f, "Sniper Rifle" },
		{ "soldier",    1.00f, 0.60f, 550.0f, 0.30f,  9.0f, "AK-47" },
		{ "hunter",     0.95f, 0.80f, 400.0f, 0.25f, 16.0f, "Tommygun" },
		{ "cautious",   0.95f, 0.35f, 700.0f, 0.55f,  6.0f, "Crossbow" },
		{ "brawler",    1.10f, 0.85f, 260.0f, 0.20f, 11.0f, "Tommygun" },
	};
	const int kNumPersonalities = sizeof( s_aPersonalities ) / sizeof( s_aPersonalities[0] );

	struct Noise { LTObjRef hSource; LTVector vPos; float fTime; float fRadius; };
	Noise s_aNoises[16];
	int s_nNextNoise = 0;
}

void CBotBrain::ReportNoise( HOBJECT hSource, const LTVector& vPos, float fRadius )
{
	Noise& n = s_aNoises[s_nNextNoise];
	s_nNextNoise = ( s_nNextNoise + 1 ) % 16;
	n.hSource = hSource;
	n.vPos = vPos;
	n.fTime = Now( );
	n.fRadius = fRadius;
}

// Did an enemy just make noise within earshot?
bool CBotBrain::HearNoise( CPlayerObj* pBot, LTVector* pvPos )
{
	LTVector vMe;
	g_pLTServer->GetObjectPos( pBot->m_hObject, &vMe );
	float fNow = Now( ), fBest = 1.0e30f;
	bool bHeard = false;
	for( int i = 0; i < 16; i++ )
	{
		const Noise& n = s_aNoises[i];
		if( !n.hSource || fNow - n.fTime > 1.0f ) continue;
		if( !IsEnemy( pBot, n.hSource )) continue;
		float fD = sqrtf( n.vPos.DistSqr( vMe ));
		if( fD > n.fRadius || fD >= fBest ) continue;
		fBest = fD;
		*pvPos = n.vPos;
		bHeard = true;
	}
	return bHeard;
}

const char* CBotBrain::SetPersonality( const char* szName )
{
	uint32 nHash = 5381;
	for( const char* p = szName; p && *p; p++ ) nHash = nHash * 33 + ( uint8 )*p;
	const Personality& per = s_aPersonalities[nHash % kNumPersonalities];
	// A little individual variation on top of the style.
	m_fAccuracy = per.fAccuracy * RandF( 0.92f, 1.08f );
	m_fAggression = per.fAggression;
	m_fPrefDist = per.fPrefDist * RandF( 0.85f, 1.15f );
	m_fRetreatHealth = per.fRetreatHealth;
	m_fHuntTime = per.fHuntTime;
	m_szFavWeapon = per.szFavWeapon;
	m_nChatStyle = ( per.fAggression > 0.65f ) ? 0 : ( per.fAggression < 0.45f ) ? 1 : 2;
	return per.szStyle;
}

CBotBrain::CBotBrain( int nSkill )
{
	m_nSkill = ( nSkill < 2 ) ? 2 : ( nSkill > 4 ) ? 4 : nSkill;
	m_nLastFrags = -99999;
	m_fGreetAt = 0.0f;
	m_nStreak = 0;
	m_fNextChat = 0.0f;
	m_nChatStyle = 2;
	m_hLastKiller = LTNULL;
	m_hLastEnemy = LTNULL;
	m_fYaw = 0.0f;
	m_fPitch = 0.0f;
	m_hTarget = LTNULL;
	m_fTargetSeenTime = 0.0f;
	m_fTargetLastSeenTime = -100.0f;
	m_vTargetLastSeenPos = LTVector( 0.0f, 0.0f, 0.0f );
	m_bHaveLastSeenPos = false;
	m_vAimError = LTVector( 0.0f, 0.0f, 0.0f );
	m_fNextAimErrorTime = 0.0f;
	m_fStrafeDir = 1.0f;
	m_fNextStrafeChange = 0.0f;
	m_fNextJumpTime = 0.0f;
	m_fNextWeaponCheck = 0.0f;
	m_fLastHealth = 0.0f;
	m_hLastAttacker = LTNULL;
	m_fLastAttackedTime = -100.0f;
	m_nPathIndex = 0;
	m_iGoalNode = -1;
	m_fNextRepathTime = 0.0f;
	m_vLastProgressPos = LTVector( 0.0f, 0.0f, 0.0f );
	m_fLastProgressTime = 0.0f;
	m_fGoalTimeout = 0.0f;
	m_fBurstEnd = 0.0f;
	m_fPauseEnd = 0.0f;
	m_fUnstuckUntil = 0.0f;
	m_vUnstuckDir = LTVector( 0.0f, 0.0f, 0.0f );
	m_hItemGoal = LTNULL;
	m_vItemPos = LTVector( 0.0f, 0.0f, 0.0f );
	m_fNextItemCheck = 0.0f;
	m_fItemGoalTimeout = 0.0f;
	m_bMeleeMode = false;
	m_vSmoothMove = LTVector( 0.0f, 0.0f, 0.0f );
	m_fNearItemSince = 0.0f;
	m_fNextRetarget = 0.0f;
	m_fNoPathSince = 0.0f;
	m_hObjectivePlayer = LTNULL;
	m_fObjectiveReplan = 0.0f;
	m_fObjectiveTimeout = 0.0f;
	m_fNextLookTime = 0.0f;
	m_fLookYaw = 0.0f;
	m_fLeanUntil = 0.0f;
	m_fLeanSide = 0.0f;
	m_fNextStrafeProbe = 0.0f;
	m_vAimErrorGoal = LTVector( 0.0f, 0.0f, 0.0f );
	m_fAccuracy = 1.0f;
	m_fAggression = 0.6f;
	m_fPrefDist = 550.0f;
	m_fRetreatHealth = 0.35f;
	m_fHuntTime = 9.0f;
	m_szFavWeapon = LTNULL;
	m_fDuckUntil = 0.0f;
	m_fNextDoorTime = 0.0f;
	m_hLastDoor = LTNULL;
	m_nDoorTries = 0;
	m_aTrail.clear( );
	m_aBack.clear( );
	m_nBackIdx = 0;
	m_fBackUntil = 0.0f;
	m_fBackLast = 0.0f;
	m_fBackProg = 0.0f;
	m_vBackLastPos = LTVector( 0.0f, 0.0f, 0.0f );
	m_fWallUntil = 0.0f;
	m_fWallNext = 0.0f;
	m_fWallProgT = 0.0f;
	m_fWallSide = 1.0f;
	m_vWallDir = LTVector( 0.0f, 0.0f, 1.0f );
	m_vWallStart = LTVector( 0.0f, 0.0f, 0.0f );
	m_vWallProgPos = LTVector( 0.0f, 0.0f, 0.0f );
	m_fLastDoorTry = 0.0f;
	m_fCombatPathUntil = 0.0f;
	m_fDropUntil = 0.0f;
	m_vDropGoal = LTVector( 0.0f, 0.0f, 0.0f );
	m_iLadder = -1;
	m_nLadderPhase = 0;
	m_fLadderStart = 0.0f;
	m_fLadderY = 0.0f;
	m_fLadderYTime = 0.0f;
	m_bLadderUp = false;
	m_fNextDuckProbe = 0.0f;
	m_fNextInvestigate = 0.0f;
	m_fInvestigateArrive = 0.0f;
	m_fNextSkipCheck = 0.0f;
	m_fNextScan = 0.0f;
	m_fNextTouch = 0.0f;
	m_bRetreating = false;
	m_fRetreatUntil = 0.0f;
	m_vInvestigatePos = LTVector( 0.0f, 0.0f, 0.0f );
	m_fInvestigateUntil = 0.0f;
	m_fStuckWindowStart = 0.0f;
	m_nStuckCount = 0;
}

float CBotBrain::GetDamageScale( ) const { return BlendedSkill( m_nSkill, s_fAdapt ).fDamageScale; }
float CBotBrain::GetFireDelayScale( ) const { return BlendedSkill( m_nSkill, s_fAdapt ).fFireDelayScale; }
float CBotBrain::GetPerturbScale( ) const { return BlendedSkill( m_nSkill, s_fAdapt ).fPerturbScale; }

CBotBrain::~CBotBrain( )
{
}

void CBotBrain::OnRespawn( CPlayerObj* pBot )
{
	m_hTarget = LTNULL;
	m_bHaveLastSeenPos = false;
	m_aPath.clear( );
	m_iGoalNode = -1;
	m_hItemGoal = LTNULL;
	m_hObjectivePlayer = LTNULL;
	m_fNoPathSince = 0.0f;
	m_bRetreating = false;
	m_fInvestigateUntil = 0.0f;
	m_fInvestigateArrive = 0.0f;
	m_fNextInvestigate = 0.0f;
	m_fDuckUntil = 0.0f;
	m_fNextDoorTime = 0.0f;
	m_hLastDoor = LTNULL;
	m_nDoorTries = 0;
	m_aTrail.clear( );
	m_aBack.clear( );
	m_nBackIdx = 0;
	m_fBackUntil = 0.0f;
	m_fBackLast = 0.0f;
	m_fBackProg = 0.0f;
	m_vBackLastPos = LTVector( 0.0f, 0.0f, 0.0f );
	m_fWallUntil = 0.0f;
	m_fWallNext = 0.0f;
	m_fWallProgT = 0.0f;
	m_fWallSide = 1.0f;
	m_vWallDir = LTVector( 0.0f, 0.0f, 1.0f );
	m_vWallStart = LTVector( 0.0f, 0.0f, 0.0f );
	m_vWallProgPos = LTVector( 0.0f, 0.0f, 0.0f );
	m_fLastDoorTry = 0.0f;
	m_fCombatPathUntil = 0.0f;
	m_fDropUntil = 0.0f;
	m_vDropGoal = LTVector( 0.0f, 0.0f, 0.0f );
	m_iLadder = -1;
	m_nLadderPhase = 0;
	m_fLadderStart = 0.0f;
	m_fLadderY = 0.0f;
	m_fLadderYTime = 0.0f;
	m_bLadderUp = false;
	m_bMeleeMode = false;
	m_fNextWeaponCheck = 0.0f;
	m_fLastHealth = pBot->BotGetHealth( );
	m_hLastAttacker = LTNULL;

	LTRotation rRot;
	g_pLTServer->GetObjectRotation( pBot->m_hObject, &rRot );
	LTVector vF = rRot.Forward( );
	m_fYaw = atan2f( vF.x, vF.z );
	m_fPitch = 0.0f;
	m_fLastProgressTime = Now( );
	g_pLTServer->GetObjectPos( pBot->m_hObject, &m_vLastProgressPos );
}

void CBotBrain::OnDeath( CPlayerObj* pBot )
{
	// Who got us?  (Last enemy to hurt us in the last few seconds.)
	const char* szKiller = LTNULL;
	if( m_hLastAttacker && Now( ) - m_fLastAttackedTime < 5.0f )
	{
		CPlayerObj* pK = dynamic_cast< CPlayerObj* >( g_pLTServer->HandleToObject( m_hLastAttacker ));
		if( pK && pK != pBot )
		{
			szKiller = pK->GetNetUniqueName( );
			m_hLastKiller = m_hLastAttacker;
		}
	}

	if( m_nStreak >= 5 && szKiller )
	{
		char sz[200];
		sprintf( sz, "Streak over... nice shooting, %s.", szKiller );
		Say( pBot, sz, true );
	}
	else if( GetRandom( 0, 99 ) < 40 )
	{
		static const char* s_aszCocky[] = { "Lucky shot, %s.", "Cheap, %s. Real cheap.", "I'll be back, %s.", "Don't get used to it, %s." };
		static const char* s_aszNice[] = { "Ow! Nice one, %s.", "You got me, %s.", "Not bad, %s.", "Good shot, %s." };
		static const char* s_aszPlain[] = { "Argh!", "Ow.", "Not again...", "Unreal.", "Lag!" };
		if( szKiller )
		{
			const char* szFmt = ( m_nChatStyle == 0 ) ? PickLine( s_aszCocky, 4 ) : ( m_nChatStyle == 1 ) ? PickLine( s_aszNice, 4 ) : ( GetRandom( 0, 1 ) ? PickLine( s_aszCocky, 4 ) : PickLine( s_aszNice, 4 ));
			char sz[200];
			sprintf( sz, szFmt, szKiller );
			Say( pBot, sz );
		}
		else
			Say( pBot, PickLine( s_aszPlain, 5 ));
	}

	m_nStreak = 0;
	m_hTarget = LTNULL;
	m_aPath.clear( );
}

// ----------------------------------------------------------------------- //
// Chat
// ----------------------------------------------------------------------- //

void CBotBrain::SetAdaptiveSkill( float f )
{
	if( f < -1.0f ) f = -1.0f;
	if( f > 1.0f ) f = 1.0f;
	s_fAdapt = f;
}

float CBotBrain::GetAdaptiveSkill( ) { return s_fAdapt; }

void CBotBrain::SetChatEnabled( bool b ) { s_bChat = b; }

void CBotBrain::Say( CPlayerObj* pBot, const char* szMsg, bool bForce )
{
	if( !s_bChat || !pBot || !szMsg || !szMsg[0] ) return;
	float fNow = Now( );
	if( !bForce && fNow < m_fNextChat ) return;
	if( s_fLastChatAny > fNow ) s_fLastChatAny = -100.0f;	// New level: the clock restarted.
	if( fNow - s_fLastChatAny < 1.5f ) return;
	s_fLastChatAny = fNow;
	m_fNextChat = fNow + RandF( 7.0f, 16.0f );

	g_pLTServer->CPrint( "bot-chat: %s : %s", pBot->GetNetUniqueName( ), szMsg );

	// Same message a player's chat produces: shows up as "Name : text" with the chat sound.
	CAutoMessage cMsg;
	cMsg.Writeuint8( MID_PLAYER_MESSAGE );
	cMsg.WriteString( szMsg );
	cMsg.Writeuint32( pBot->GetPlayerClientID( ));
	cMsg.Writeuint8( INVALID_TEAM );
	g_pLTServer->SendToClient( cMsg.Read( ), LTNULL, MESSAGE_GUARANTEED );
}

void CBotBrain::Greet( CPlayerObj* pBot )
{
	m_fGreetAt = Now( ) + RandF( 4.0f, 9.0f );
}

void CBotBrain::DoGreet( CPlayerObj* pBot )
{
	if( GetRandom( 0, 99 ) >= 70 ) return;
	static const char* s_aszCocky[] = { "Time to lose, humans.", "Let's see what you've got.", "Fresh meat!" };
	static const char* s_aszNice[] = { "Hey all, good luck!", "Hi! Have fun.", "Ready when you are." };
	static const char* s_aszPlain[] = { "Hey all.", "Let's do this.", "Here we go." };
	Say( pBot, ( m_nChatStyle == 0 ) ? PickLine( s_aszCocky, 3 ) : ( m_nChatStyle == 1 ) ? PickLine( s_aszNice, 3 ) : PickLine( s_aszPlain, 3 ), true );
}

void CBotBrain::OnFrag( CPlayerObj* pBot )
{
	// Who did we just get?  (Our last target, if it is dead now.)
	const char* szVictim = LTNULL;
	bool bRevenge = false;
	CPlayerObj* pV = m_hLastEnemy ? dynamic_cast< CPlayerObj* >( g_pLTServer->HandleToObject( m_hLastEnemy )) : LTNULL;
	if( pV && pV != pBot && ( pV->GetState( ) != PS_ALIVE || pV->BotIsDead( )))
	{
		szVictim = pV->GetNetUniqueName( );
		bRevenge = m_hLastKiller && ( HOBJECT )m_hLastKiller == pV->m_hObject;
		if( bRevenge ) m_hLastKiller = LTNULL;
	}

	const char* szSpree = LTNULL;
	if( m_nStreak == 3 ) szSpree = "I'm on a killing spree!";
	else if( m_nStreak == 5 ) szSpree = "Rampage! Five down!";
	else if( m_nStreak == 7 ) szSpree = "Dominating!";
	else if( m_nStreak == 10 ) szSpree = "GODLIKE!";
	if( szSpree )
	{
		Say( pBot, szSpree, true );
		return;
	}

	if( GetRandom( 0, 99 ) >= 45 ) return;

	char sz[200];
	if( szVictim && bRevenge )
	{
		static const char* s_aszRevenge[] = { "Payback, %s.", "That's for earlier, %s.", "We're even now, %s." };
		sprintf( sz, PickLine( s_aszRevenge, 3 ), szVictim );
	}
	else if( szVictim )
	{
		static const char* s_aszCocky[] = { "Too easy, %s.", "Sit down, %s.", "Is that all you've got, %s?", "Owned, %s.", "Did you even see me, %s?", "Better luck next time, %s." };
		static const char* s_aszNice[] = { "Good fight, %s.", "Close one, %s!", "Sorry about that, %s.", "Gotcha, %s!", "Nice try, %s." };
		static const char* s_aszPlain[] = { "Nice try, %s.", "Got you, %s.", "One down, %s.", "Not today, %s.", "That's one, %s." };
		const char* szFmt = ( m_nChatStyle == 0 ) ? PickLine( s_aszCocky, 6 ) : ( m_nChatStyle == 1 ) ? PickLine( s_aszNice, 5 ) : PickLine( s_aszPlain, 5 );
		sprintf( sz, szFmt, szVictim );
	}
	else
	{
		static const char* s_aszNoName[] = { "Boom.", "Next!", "Gotcha!", "Easy.", "Heh.", "Who's next?" };
		sprintf( sz, "%s", PickLine( s_aszNoName, 6 ));
	}
	Say( pBot, sz );
}

// ----------------------------------------------------------------------- //
// Senses
// ----------------------------------------------------------------------- //

bool CBotBrain::IsEnemy( CPlayerObj* pBot, HOBJECT hOther )
{
	if( !hOther || hOther == pBot->m_hObject ) return false;
	CPlayerObj* pOther = dynamic_cast< CPlayerObj* >( g_pLTServer->HandleToObject( hOther ));
	if( !pOther ) return false;
	if( pOther->GetState( ) != PS_ALIVE || pOther->IsDead( )) return false;
	if( IsTeamGameType( ) && pOther->GetTeamID( ) == pBot->GetTeamID( )) return false;
	return true;
}

// Line of sight to the head or the chest of the target.
bool CBotBrain::CanSee( const LTVector& vEye, HOBJECT hTarget, LTVector* pvAimPoint )
{
	LTVector vPos, vDims;
	g_pLTServer->GetObjectPos( hTarget, &vPos );
	g_pPhysicsLT->GetObjectDims( hTarget, &vDims );

	LTVector aPoints[2] = { vPos + LTVector( 0.0f, vDims.y * 0.35f, 0.0f ), vPos + LTVector( 0.0f, vDims.y * 0.8f, 0.0f ) };
	for( int i = 0; i < 2; i++ )
	{
		IntersectQuery iq;
		IntersectInfo ii;
		iq.m_From = vEye;
		iq.m_To = aPoints[i];
		iq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
		iq.m_FilterFn = ViewBlockerFn;
		if( !g_pLTServer->IntersectSegment( &iq, &ii ))
		{
			if( pvAimPoint ) *pvAimPoint = aPoints[i];
			return true;
		}
	}
	return false;
}

HOBJECT CBotBrain::FindVisibleEnemy( CPlayerObj* pBot, const LTVector& vEye, float fYaw )
{
	const SkillData sk = BlendedSkill( m_nSkill, s_fAdapt );
	HOBJECT hBest = LTNULL;
	float fBestScore = 1.0e30f;

	CPlayerObj::PlayerObjList::const_iterator it = CPlayerObj::GetPlayerObjList( ).begin( );
	for( ; it != CPlayerObj::GetPlayerObjList( ).end( ); ++it )
	{
		CPlayerObj* pOther = *it;
		if( !pOther || !IsEnemy( pBot, pOther->m_hObject )) continue;

		LTVector vPos;
		g_pLTServer->GetObjectPos( pOther->m_hObject, &vPos );
		LTVector vTo = vPos - vEye;
		float fDist = vTo.Mag( );
		if( fDist > 5000.0f ) continue;
		if( !( pOther->m_hObject == ( HOBJECT )m_hLastAttacker && Now( ) - m_fLastAttackedTime < 3.0f ) && OnOtherFloor( vEye, vPos )) continue;

		// Field of view, except for whoever just shot us (we turn to them).
		bool bAttacker = ( pOther->m_hObject == ( HOBJECT )m_hLastAttacker && Now( ) - m_fLastAttackedTime < 3.0f );
		float fAng = fabs( WrapAngle( atan2f( vTo.x, vTo.z ) - fYaw ));
		if( !bAttacker && fAng > sk.fFov ) continue;

		if( !CanSee( vEye, pOther->m_hObject, LTNULL )) continue;

		// Real players are what bots care about; other bots are a distant second.
		float fScore = fDist + fAng * 400.0f - ( bAttacker ? 800.0f : 0.0f ) - ( pOther->GetClient( ) ? 1500.0f : 0.0f );
		if( fScore < fBestScore )
		{
			fBestScore = fScore;
			hBest = pOther->m_hObject;
		}
	}
	return hBest;
}

// ----------------------------------------------------------------------- //
// Navigation
// ----------------------------------------------------------------------- //

bool CBotBrain::PlanPathToNode( const LTVector& vFeet, int iGoal )
{
	m_aPath.clear( );
	m_nPathIndex = 0;
	m_iGoalNode = iGoal;
	if( iGoal < 0 ) return false;
	// Try the nearest few nodes with a clear line: the closest may sit in a pocket
	// (crate top, shelf) that has no connection to the rest of the map.
	int iStart = -1;
	bool bFarStart = false;
	bool bFound = false;
	for( int nSkip = 0; nSkip < 6 && !bFound; nSkip++ )
	{
		bool bFar = false;
		int iTry = BotNav::FindVisibleNode( vFeet, 200.0f, nSkip );
		if( iTry < 0 )
		{
			iTry = BotNav::FindVisibleNode( vFeet, 600.0f, nSkip );
			bFar = true;
		}
		if( iTry < 0 ) break;
		if( BotNav::FindPath( iTry, iGoal, m_aPath ))
		{
			iStart = iTry;
			bFarStart = bFar;
			bFound = true;
		}
	}
	if( !bFound ) return false;
	m_nPathIndex = ( m_aPath.size( ) > 1 && !bFarStart ) ? 1 : 0;
	m_fGoalTimeout = Now( ) + 30.0f;
	return true;
}

bool CBotBrain::PlanPathTo( const LTVector& vFeet, const LTVector& vGoalFeet )
{
	return PlanPathToNode( vFeet, BotNav::FindNearestNode( vGoalFeet, 300.0f ));
}

void CBotBrain::PickRoamGoal( const LTVector& vFeet )
{
	// Prefer goals some distance away so bots actually roam the map.
	for( int i = 0; i < 4; i++ )
	{
		int iNode = BotNav::RandomNode( );
		if( iNode < 0 ) return;
		LTVector v = BotNav::GetNodePos( iNode );
		if( v.DistSqr( vFeet ) < 600.0f * 600.0f && i < 3 ) continue;
		if( IsBadSpot( v ) && i < 3 ) continue;
		if( PlanPathToNode( vFeet, iNode )) return;
	}
}

// Steers along the current path.  Returns false when there's nothing to follow.
bool CBotBrain::FollowPath( const LTVector& vFeet, LTVector* pvMoveDir )
{
	if( m_nPathIndex >= m_aPath.size( ))
		return false;

	// Skip waypoints we've reached, and cut corners when the way ahead is clear.
	while( m_nPathIndex < m_aPath.size( ))
	{
		LTVector vWp = BotNav::GetNodePos( m_aPath[m_nPathIndex] );
		float dx = vWp.x - vFeet.x, dz = vWp.z - vFeet.z;
		if( dx * dx + dz * dz < 28.0f * 28.0f && fabs( vWp.y - vFeet.y ) < 48.0f )
		{
			m_nPathIndex++;
			continue;
		}
		bool bSkipped = false;
		if( Now( ) < m_fNextSkipCheck ) break;	// Ray checks a few times a second is plenty.
		m_fNextSkipCheck = Now( ) + 0.2f;
		for( uint32 nAhead = 3; nAhead >= 1 && !bSkipped; nAhead-- )
		{
			if( m_nPathIndex + nAhead >= m_aPath.size( )) continue;
			LTVector vNext = BotNav::GetNodePos( m_aPath[m_nPathIndex + nAhead] );
			if( fabs( vNext.y - vFeet.y ) > 16.0f || fabs( vWp.y - vFeet.y ) > 16.0f ) continue;

			// Clear straight line for the whole body: centre and both shoulders,
			// at foot and chest height.
			bool bClear = true;
			LTVector vDirH = vNext - vFeet;
			vDirH.y = 0.0f;
			float fDirLen = sqrtf( vDirH.x * vDirH.x + vDirH.z * vDirH.z );
			if( fDirLen < 1.0f ) continue;
			LTVector vSide( vDirH.z / fDirLen, 0.0f, -vDirH.x / fDirLen );
			for( int k = 0; k < 6 && bClear; k++ )
			{
				IntersectQuery iq;
				IntersectInfo ii;
				float fH = ( k < 3 ) ? 14.0f : 42.0f;
				float fOff = ( float )(( k % 3 ) - 1 ) * 26.0f;
				iq.m_From = vFeet + LTVector( 0.0f, fH, 0.0f ) + vSide * fOff;
				iq.m_To = vNext + LTVector( 0.0f, fH, 0.0f ) + vSide * fOff;
				iq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
				iq.m_FilterFn = SightFilterFn;
				if( g_pLTServer->IntersectSegment( &iq, &ii )) bClear = false;
			}
			for( uint32 m = 0; m < nAhead && bClear; m++ )
				if( fabs( BotNav::GetNodePos( m_aPath[m_nPathIndex + m] ).y - vNext.y ) > 16.0f ) bClear = false;

			if( bClear )
			{
				m_nPathIndex += nAhead;
				bSkipped = true;
			}
		}
		if( bSkipped ) continue;
		break;
	}

	if( m_nPathIndex >= m_aPath.size( ))
		return false;

	LTVector vWp = BotNav::GetNodePos( m_aPath[m_nPathIndex] );
	LTVector vDir = vWp - vFeet;
	vDir.y = 0.0f;
	float fLen = sqrtf( vDir.x * vDir.x + vDir.z * vDir.z );
	if( fLen < 0.001f ) return false;
	*pvMoveDir = vDir * ( 1.0f / fLen );
	return true;
}

// ----------------------------------------------------------------------- //
// Weapons
// ----------------------------------------------------------------------- //

void CBotBrain::ChooseWeapon( CPlayerObj* pBot, float fTargetDist )
{
	if( Now( ) < m_fNextWeaponCheck ) return;
	m_fNextWeaponCheck = Now( ) + (( fTargetDist > 0.0f ) ? 0.4f : 1.0f );

	// Preference by name (player weapons only).
	static const char* s_aszMelee[] = { "Fists", "Shotgun", "Tommygun", "AK-47", "Sterling", "SilencedSterling", "Desert Eagle", "Beretta", LTNULL };
	static const char* s_aszClose[] = { "Shotgun", "Tommygun", "AK-47", "Sterling", "SilencedSterling", "Desert Eagle", "Beretta", "Fists", LTNULL };
	static const char* s_aszMid[]   = { "AK-47", "Tommygun", "Crossbow", "Sterling", "SilencedSterling", "Desert Eagle", "Beretta", "Shotgun", "Fists", LTNULL };
	static const char* s_aszFar[]   = { "Sniper Rifle", "AK-47", "Crossbow", "Tommygun", "Sterling", "SilencedSterling", "Desert Eagle", "Beretta", "Shotgun", "Fists", LTNULL };

	// Players switch to melee when someone is right in their face, and back
	// once there is some space again (hysteresis so we don't flip-flop).
	if( fTargetDist > 0.0f )
	{
		if( fTargetDist < 110.0f ) m_bMeleeMode = true;
		else if( fTargetDist > 220.0f ) m_bMeleeMode = false;
	}
	else
		m_bMeleeMode = false;

	const char** aszList;
	if( fTargetDist <= 0.0f )			aszList = s_aszMid;		// Nobody around: ready for anything.
	else if( m_bMeleeMode )			aszList = s_aszMelee;
	else if( fTargetDist < 450.0f )	aszList = s_aszClose;
	else if( fTargetDist > 1400.0f )	aszList = s_aszFar;
	else								aszList = s_aszMid;

	// Its favourite gun goes first when the range suits it.
	const char* aszWithFav[16];
	int nW = 0;
	if( m_szFavWeapon && !m_bMeleeMode )
	{
		bool bLong = ( strstr( m_szFavWeapon, "Sniper" ) || strstr( m_szFavWeapon, "Crossbow" ));
		bool bShort = ( strstr( m_szFavWeapon, "Shotgun" ) != LTNULL );
		bool bOk = fTargetDist <= 0.0f || ( bLong ? fTargetDist > 450.0f : bShort ? fTargetDist < 700.0f : fTargetDist < 1600.0f );
		if( bOk ) aszWithFav[nW++] = m_szFavWeapon;
	}
	for( int i = 0; aszList[i] && nW < 15; i++ ) aszWithFav[nW++] = aszList[i];
	aszWithFav[nW] = LTNULL;
	aszList = aszWithFav;

	CWeapon* pCur = pBot->BotGetCurrentWeapon( );
	for( int i = 0; aszList[i]; i++ )
	{
		WEAPON const* pData = g_pWeaponMgr->GetWeapon( aszList[i] );
		if( !pData ) continue;
		CWeapon* pW = pBot->BotGetWeapon(( uint8 )pData->nId );
		if( !pW || !pW->Have( )) continue;

		bool bHasAmmo = pData->bInfiniteAmmo || pW->GetAmmoInClip( ) > 0;
		for( int a = 0; !bHasAmmo && a < pData->nNumAmmoIds; a++ )
			bHasAmmo = pBot->BotGetAmmoCount( pData->aAmmoIds[a] ) > 0;
		if( !bHasAmmo ) continue;

		if( pCur && pCur->GetId( ) == pData->nId ) return;	// Already the best.
		pBot->BotSelectWeapon(( uint8 )pData->nId );
		return;
	}
}

// ----------------------------------------------------------------------- //
// Pickups
// ----------------------------------------------------------------------- //

bool CBotBrain::RescueTeleport( CPlayerObj* pBot, const LTVector& vPos, float fMinDist )
{
	// Put a trapped bot on a node that really connects to the rest of the map,
	// at least fMinDist away (horizontally) from where it is.
	for( int nTry = 0; nTry < 40; nTry++ )
	{
		int iCand = BotNav::RandomNode( );
		if( iCand < 0 ) break;
		LTVector vN = BotNav::GetNodePos( iCand );
		float dx = vN.x - vPos.x, dz = vN.z - vPos.z;
		if( dx * dx + dz * dz < fMinDist * fMinDist ) continue;
		std::vector<int> aTest;
		if( !BotNav::FindPath( iCand, BotNav::RandomNode( ), aTest ) || aTest.size( ) <= 20 ) continue;
		float fHalf = pBot->BotStandHalfHeight( );
		if( fHalf <= 0.0f ) fHalf = 54.0f;
		vN.y += fHalf + 2.0f;
		g_pLTServer->SetObjectPos( pBot->m_hObject, &vN );
		g_pLTServer->CPrint( "bot-rescue: %s trapped at <%.0f,%.0f,%.0f>, moved to node %d", pBot->GetNetUniqueName( ), vPos.x, vPos.y, vPos.z, iCand );
		m_aPath.clear( );
		m_aTrail.clear( );
		m_fBackUntil = 0.0f;
		m_fLastProgressTime = Now( );
		m_vLastProgressPos = vN;
		m_fNextRepathTime = 0.0f;
		return true;
	}
	return false;
}

void CBotBrain::RememberBadSpot( const LTVector& vPos )
{
	float fNow = Now( );
	for( size_t i = 0; i < m_aBadSpots.size( ); i++ )
	{
		if( m_aBadSpots[i].fUntil <= fNow )
		{
			m_aBadSpots[i].vPos = vPos;
			m_aBadSpots[i].fUntil = fNow + 90.0f;
			return;
		}
	}
	if( m_aBadSpots.size( ) < 12 )
	{
		BadSpot b;
		b.vPos = vPos;
		b.fUntil = fNow + 90.0f;
		m_aBadSpots.push_back( b );
	}
}

bool CBotBrain::IsBadSpot( const LTVector& vPos )
{
	float fNow = Now( );
	for( size_t i = 0; i < m_aBadSpots.size( ); i++ )
		if( m_aBadSpots[i].fUntil > fNow && m_aBadSpots[i].vPos.DistSqr( vPos ) < 300.0f * 300.0f )
			return true;
	return false;
}

bool CBotBrain::IsItemIgnored( HOBJECT hItem )
{
	float fNow = Now( );
	for( size_t i = 0; i < m_aIgnoredItems.size( ); i++ )
		if( m_aIgnoredItems[i].hItem == hItem && m_aIgnoredItems[i].fUntil > fNow )
			return true;
	return false;
}

void CBotBrain::IgnoreItem( HOBJECT hItem, float fSeconds )
{
	float fNow = Now( );
	for( size_t i = 0; i < m_aIgnoredItems.size( ); i++ )
	{
		if( m_aIgnoredItems[i].hItem == hItem || m_aIgnoredItems[i].fUntil <= fNow )
		{
			m_aIgnoredItems[i].hItem = hItem;
			m_aIgnoredItems[i].fUntil = fNow + fSeconds;
			return;
		}
	}
	IgnoredItem e;
	e.hItem = hItem;
	e.fUntil = fNow + fSeconds;
	m_aIgnoredItems.push_back( e );
}

// Running over an item picks it up, just like walking over it as a player.
void CBotBrain::TryTouchNearbyItems( CPlayerObj* pBot, const LTVector& vPos )
{
	const std::vector< PickupItem* >& aItems = PickupItem::GetPickupList( );
	for( size_t i = 0; i < aItems.size( ); i++ )
	{
		PickupItem* pItem = aItems[i];
		if( !pItem || !pItem->IsFullyCreated( ) || pItem->WasPickedUp( )) continue;
		HOBJECT hItem = pItem->m_hObject;
		if( !hItem || IsItemIgnored( hItem )) continue;

		LTVector vItem;
		g_pLTServer->GetObjectPos( hItem, &vItem );
		float dx = vItem.x - vPos.x, dz = vItem.z - vPos.z;
		if( dx * dx + dz * dz > 56.0f * 56.0f || fabsf( vItem.y - vPos.y ) > 90.0f ) continue;

		pBot->SetActivatingPickup( true );
		pItem->ObjectTouch( pBot->m_hObject );
		pBot->SetActivatingPickup( false );

		// If it wasn't taken (we're full, weapons-stay...) don't keep trying.
		IgnoreItem( hItem, pItem->WasPickedUp( ) ? 1.0f : 25.0f );
		if(( HOBJECT )m_hItemGoal == hItem ) m_hItemGoal = LTNULL;
		return;
	}
}

// Picks the most useful nearby item and plans a route to it.
void CBotBrain::SeekItems( CPlayerObj* pBot, const LTVector& vFeet, bool bHealthOnly )
{
	float fNow = Now( );

	// Drop a goal that's gone, taken, or taking too long.
	if( m_hItemGoal )
	{
		PickupItem* pGoal = dynamic_cast< PickupItem* >( g_pLTServer->HandleToObject( m_hItemGoal ));
		if( !pGoal || pGoal->WasPickedUp( ) || IsItemIgnored( m_hItemGoal ))
		{
			m_hItemGoal = LTNULL;
			m_aPath.clear( );
		}
		else if( fNow > m_fItemGoalTimeout )
		{
			IgnoreItem( m_hItemGoal, 12.0f );
			m_hItemGoal = LTNULL;
			m_aPath.clear( );
		}
	}
	if( m_hItemGoal || fNow < m_fNextItemCheck ) return;
	m_fNextItemCheck = fNow + 1.0f;

	float fHealthFrac = pBot->BotGetMaxHealth( ) > 0.0f ? pBot->BotGetHealth( ) / pBot->BotGetMaxHealth( ) : 1.0f;

	PickupItem* pBest = LTNULL;
	float fBestScore = 0.0f;
	LTVector vBestPos( 0.0f, 0.0f, 0.0f );

	const std::vector< PickupItem* >& aItems = PickupItem::GetPickupList( );
	for( size_t i = 0; i < aItems.size( ); i++ )
	{
		PickupItem* pItem = aItems[i];
		if( !pItem || !pItem->IsFullyCreated( ) || pItem->WasPickedUp( )) continue;
		HOBJECT hItem = pItem->m_hObject;
		if( !hItem || IsItemIgnored( hItem )) continue;

		if( bHealthOnly && !dynamic_cast< GearItem* >( pItem )) continue;

		// How much do we want it?
		float fValue = 0.5f;
		if( WeaponItem* pWI = dynamic_cast< WeaponItem* >( pItem ))
		{
			CWeapon* pW = pBot->BotGetWeapon( pWI->GetWeaponId( ));
			fValue = ( pW && pW->Have( )) ? 1.0f : 3.0f;
		}
		else if( dynamic_cast< AmmoBox* >( pItem ))
			fValue = 1.2f;
		else if( dynamic_cast< GearItem* >( pItem ))
			fValue = ( fHealthFrac < 0.6f ) ? 3.0f : 1.5f;
		else if( dynamic_cast< ModItem* >( pItem ))
			fValue = 0.7f;

		LTVector vItem;
		g_pLTServer->GetObjectPos( hItem, &vItem );
		float fDist = sqrtf( vItem.DistSqr( vFeet ));
		if( fDist > 2500.0f || IsBadSpot( vItem )) continue;

		float fScore = fValue / ( fDist + 300.0f );
		if( fScore > fBestScore )
		{
			fBestScore = fScore;
			pBest = pItem;
			vBestPos = vItem;
		}
	}

	if( !pBest ) return;

	if( PlanPathTo( vFeet, vBestPos - LTVector( 0.0f, 20.0f, 0.0f )))
	{
		m_hItemGoal = pBest->m_hObject;
		m_vItemPos = vBestPos;
		m_fItemGoalTimeout = fNow + 25.0f;
	}
	else
	{
		IgnoreItem( pBest->m_hObject, 15.0f );	// Can't get there.
	}
}

// ----------------------------------------------------------------------- //

// Decide to go and find somebody: like a player heading to where the action is.
bool CBotBrain::PickPlayerObjective( CPlayerObj* pBot, const LTVector& vFeet )
{
	std::vector< CPlayerObj* > aHumans, aOthers;
	CPlayerObj::PlayerObjList::const_iterator it = CPlayerObj::GetPlayerObjList( ).begin( );
	for( ; it != CPlayerObj::GetPlayerObjList( ).end( ); ++it )
	{
		CPlayerObj* p = *it;
		if( !p || !IsEnemy( pBot, p->m_hObject )) continue;
		( p->GetClient( ) ? aHumans : aOthers ).push_back( p );
	}
	std::vector< CPlayerObj* >& aPick = ( !aHumans.empty( ) && ( aOthers.empty( ) || GetRandom( 0, 99 ) < 75 )) ? aHumans : aOthers;
	if( aPick.empty( )) return false;
	CPlayerObj* pWho = aPick[GetRandom( 0, ( int )aPick.size( ) - 1 )];
	if( !PlanPathTo( vFeet, FeetOf( pWho->m_hObject ))) return false;
	m_hObjectivePlayer = pWho->m_hObject;
	m_fObjectiveReplan = Now( ) + 4.0f;
	m_fObjectiveTimeout = Now( ) + 25.0f;
	return true;
}

// Feelers at both shoulders: if one side is about to clip a door frame or
// corner, lean away from it.
void CBotBrain::AvoidWalls( const LTVector& vFeet, LTVector* pvMoveDir )
{
	LTVector vDir = *pvMoveDir;
	vDir.y = 0.0f;
	float fLen = sqrtf( vDir.x * vDir.x + vDir.z * vDir.z );
	if( fLen < 0.01f ) return;
	vDir = vDir * ( 1.0f / fLen );
	LTVector vSide( vDir.z, 0.0f, -vDir.x );

	// Keep a lean for a moment once we start one, so we don't wobble.
	if( Now( ) < m_fLeanUntil && m_fLeanSide != 0.0f )
	{
		LTVector vLean = vDir + vSide * ( m_fLeanSide * 0.8f );
		*pvMoveDir = vLean * ( 1.0f / sqrtf( vLean.x * vLean.x + vLean.z * vLean.z ));
		return;
	}
	m_fLeanSide = 0.0f;

	bool abHit[2] = { false, false };
	for( int s = 0; s < 2; s++ )
	{
		float fSign = s ? 1.0f : -1.0f;
		for( int k = 0; k < 2 && !abHit[s]; k++ )
		{
			IntersectQuery iq;
			IntersectInfo ii;
			iq.m_From = vFeet + LTVector( 0.0f, k ? 50.0f : 22.0f, 0.0f ) + vSide * ( fSign * 12.0f );
			iq.m_To = iq.m_From + vDir * 40.0f + vSide * ( fSign * 10.0f );
			iq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
			iq.m_FilterFn = SightFilterFn;
			abHit[s] = !!g_pLTServer->IntersectSegment( &iq, &ii );
		}
	}
	if( abHit[0] == abHit[1] ) return;		// Clear, or a wall straight ahead (unstuck handles that).
	m_fLeanSide = abHit[0] ? 1.0f : -1.0f;
	m_fLeanUntil = Now( ) + 0.35f;
	LTVector vNew = vDir + vSide * ( m_fLeanSide * 0.8f );
	float fN = sqrtf( vNew.x * vNew.x + vNew.z * vNew.z );
	*pvMoveDir = vNew * ( 1.0f / fN );
}

void CBotBrain::TurnTowards( float fWantYaw, float fWantPitch, float fFrameTime, float fTurnRate )
{
	float fMax = fTurnRate * fFrameTime;
	// Ease in like a hand on a mouse: fast for big turns, settling gently
	// instead of snapping back and forth around the goal.
	float fEase = fFrameTime * 7.0f;
	if( fEase > 1.0f ) fEase = 1.0f;
	float dy = WrapAngle( fWantYaw - m_fYaw ) * fEase;
	if( dy > fMax ) dy = fMax; else if( dy < -fMax ) dy = -fMax;
	m_fYaw = WrapAngle( m_fYaw + dy );

	float dp = ( fWantPitch - m_fPitch ) * fEase;
	if( dp > fMax ) dp = fMax; else if( dp < -fMax ) dp = -fMax;
	m_fPitch += dp;
	if( m_fPitch > 1.4f ) m_fPitch = 1.4f;
	if( m_fPitch < -1.4f ) m_fPitch = -1.4f;
}

void CBotBrain::Update( CPlayerObj* pBot, float fFrameTime )
{
	const SkillData sk = BlendedSkill( m_nSkill, s_fAdapt );
	float fNow = Now( );

	if( m_fGreetAt > 0.0f && fNow >= m_fGreetAt )
	{
		m_fGreetAt = 0.0f;
		DoGreet( pBot );
	}

	// Remember who we were fighting, and notice when we score (for taunts).
	if( m_hTarget ) m_hLastEnemy = m_hTarget;
	{
		int nFrags = pBot->GetPlayerScore( )->GetFrags( );
		if( m_nLastFrags == -99999 )
			m_nLastFrags = nFrags;
		if( nFrags > m_nLastFrags )
		{
			m_nStreak += nFrags - m_nLastFrags;
			m_nLastFrags = nFrags;
			OnFrag( pBot );
		}
		else if( nFrags < m_nLastFrags )
		{
			m_nLastFrags = nFrags;		// Suicide / penalty.
			m_nStreak = 0;
		}
	}

	CPlayerObj::BotInput input;
	input.bRun = true;

	LTVector vPos;
	g_pLTServer->GetObjectPos( pBot->m_hObject, &vPos );
	LTVector vFeet = FeetOf( pBot->m_hObject );
	LTVector vEye = pBot->BotGetEyePos( );

	// Getting hurt makes us notice the attacker.
	float fHealth = pBot->BotGetHealth( );
	if( fHealth < m_fLastHealth - 0.5f )
	{
		HOBJECT hDamager = pBot->BotGetLastDamager( );
		if( hDamager && IsEnemy( pBot, hDamager ))
		{
			m_hLastAttacker = hDamager;
			m_fLastAttackedTime = fNow;
		}
	}
	m_fLastHealth = fHealth;

	// Break off and heal when badly hurt; come back once patched up.
	{
		float fFrac = pBot->BotGetMaxHealth( ) > 0.0f ? fHealth / pBot->BotGetMaxHealth( ) : 1.0f;
		if( !m_bRetreating && fFrac < m_fRetreatHealth )
		{
			m_bRetreating = true;
			m_fRetreatUntil = fNow + 15.0f;
			if( GetRandom( 0, 99 ) < 50 )
			{
				static const char* s_aszHurt[] = { "I need health!", "Backing off, need a medkit.", "Running low...", "Cover me!" };
				Say( pBot, PickLine( s_aszHurt, 4 ));
			}
			m_hItemGoal = LTNULL;
			m_hObjectivePlayer = LTNULL;
			m_aPath.clear( );
		}
		else if( m_bRetreating && ( fFrac > m_fRetreatHealth + 0.3f || fNow > m_fRetreatUntil ))
			m_bRetreating = false;
	}

	// Anything we run over gets picked up.
	if( fNow >= m_fNextTouch )
	{
		m_fNextTouch = fNow + 0.1f;
		TryTouchNearbyItems( pBot, vPos );
	}

	// ---- Targeting ----
	if( m_hTarget && !IsEnemy( pBot, m_hTarget ))
		m_hTarget = LTNULL;

	LTVector vAimPoint( 0.0f, 0.0f, 0.0f );
	bool bTargetVisible = false;
	if( m_hTarget )
		bTargetVisible = CanSee( vEye, m_hTarget, &vAimPoint );

	if( !bTargetVisible && fNow >= m_fNextScan )
	{
		m_fNextScan = fNow + 0.12f;
		HOBJECT hNew = FindVisibleEnemy( pBot, vEye, m_fYaw );
		if( hNew && hNew != ( HOBJECT )m_hTarget )
		{
			m_hTarget = hNew;
			m_fTargetSeenTime = fNow;
			m_fNextAimErrorTime = 0.0f;
		}
		if( m_hTarget )
			bTargetVisible = CanSee( vEye, m_hTarget, &vAimPoint );
	}

	// A target that has drifted well outside our view cone is no longer 'seen'.
	if( bTargetVisible && m_hTarget )
	{
		LTVector vTP;
		g_pLTServer->GetObjectPos( m_hTarget, &vTP );
		float fTA = fabs( WrapAngle( atan2f( vTP.x - vEye.x, vTP.z - vEye.z ) - m_fYaw ));
		bool bAtt = ( ( HOBJECT )m_hTarget == ( HOBJECT )m_hLastAttacker && fNow - m_fLastAttackedTime < 3.0f );
		if( !bAtt && fTA > sk.fFov * 1.7f )
			bTargetVisible = false;
		if( !bAtt && OnOtherFloor( vEye, vTP ))
		{
			bTargetVisible = false;
			m_hTarget = LTNULL;
		}
	}

	// Busy with another bot?  Switch to a human we can see.
	if( bTargetVisible && fNow >= m_fNextRetarget )
	{
		m_fNextRetarget = fNow + 0.7f;
		CPlayerObj* pT = dynamic_cast< CPlayerObj* >( g_pLTServer->HandleToObject( m_hTarget ));
		if( pT && !pT->GetClient( ))
		{
			HOBJECT hH = FindVisibleEnemy( pBot, vEye, m_fYaw );
			CPlayerObj* pH = hH ? dynamic_cast< CPlayerObj* >( g_pLTServer->HandleToObject( hH )) : LTNULL;
			if( pH && pH->GetClient( ) && CanSee( vEye, hH, &vAimPoint ))
			{
				m_hTarget = hH;
				m_fTargetSeenTime = fNow;
				m_fNextAimErrorTime = 0.0f;
			}
		}
	}

	if( bTargetVisible )
	{
		m_fTargetLastSeenTime = fNow;
		g_pLTServer->GetObjectPos( m_hTarget, &m_vTargetLastSeenPos );
		m_bHaveLastSeenPos = true;
	}
	else if( m_hTarget && fNow - m_fTargetLastSeenTime > m_fHuntTime )
	{
		m_hTarget = LTNULL;		// Lost them.
	}

	LTVector vMoveDir( 0.0f, 0.0f, 0.0f );
	bool bHaveMove = false;

	if( m_hTarget && bTargetVisible )
	{
		// ---- Combat ----
		float fDist = sqrtf( vAimPoint.DistSqr( vEye ));
		ChooseWeapon( pBot, fDist );

		// Aim wobble that gets re-rolled now and then (and shrinks as we track).
		if( fNow >= m_fNextAimErrorTime )
		{
			float fTrack = fNow - m_fTargetSeenTime;
			float fScale = sk.fAimError * m_fAccuracy * ( 0.25f + fDist / 1000.0f ) * (( fTrack < 1.5f ) ? 1.5f : 1.0f );
			m_vAimErrorGoal = LTVector( RandF( -fScale, fScale ), RandF( -fScale, fScale ) * 0.6f, RandF( -fScale, fScale ));
			m_fNextAimErrorTime = fNow + RandF( 0.4f, 0.9f );
		}
		{
			// Drift toward the new error rather than jumping to it.
			float fB = fFrameTime * 4.0f;
			if( fB > 1.0f ) fB = 1.0f;
			m_vAimError = m_vAimError + ( m_vAimErrorGoal - m_vAimError ) * fB;
		}

		LTVector vAt = vAimPoint + m_vAimError;
		float fWantYaw, fWantPitch;
		DirToAngles( vAt - vEye, &fWantYaw, &fWantPitch );
		TurnTowards( fWantYaw, fWantPitch, fFrameTime, sk.fTurnRate );

		// Fire once we've had time to react and the crosshair is close enough.
		bool bReacted = ( fNow - m_fTargetSeenTime ) >= sk.fReaction;
		LTRotation rAim( m_fPitch, m_fYaw, 0.0f );
		LTVector vAimDir = rAim.Forward( );
		LTVector vToTarget = vAimPoint - vEye;
		float fAlong = vToTarget.Dot( vAimDir );
		LTVector vMiss = vToTarget - vAimDir * fAlong;
		float fCone = sk.fFireCone * (( fDist > 500.0f ) ? fDist / 500.0f : 1.0f );
		bool bOnTarget = bReacted && fAlong > 0.0f && vMiss.Mag( ) < fCone;

		// Burst fire: weaker bots shoot in short bursts with pauses, like a player
		// who is not holding the trigger down.
		if( sk.fBurstMax > 0.0f )
		{
			if( fNow >= m_fBurstEnd && fNow >= m_fPauseEnd )
			{
				m_fBurstEnd = fNow + RandF( sk.fBurstMin, sk.fBurstMax );
				m_fPauseEnd = m_fBurstEnd + RandF( sk.fPauseMin, sk.fPauseMax );
			}
			if( fNow >= m_fBurstEnd ) bOnTarget = false;
		}
		input.bFire = bOnTarget;

		// Strafe and keep a sensible distance, like a player dodging.
		if( fNow >= m_fNextStrafeChange )
		{
			m_fStrafeDir = ( GetRandom( 0, 1 ) == 0 ) ? -1.0f : 1.0f;
			m_fNextStrafeChange = fNow + RandF( 0.9f, 2.2f );
		}
		LTVector vFwd( sinf( m_fYaw ), 0.0f, cosf( m_fYaw ));
		LTVector vRight( cosf( m_fYaw ), 0.0f, -sinf( m_fYaw ));
		vMoveDir = vRight * m_fStrafeDir;
		if( fDist > m_fPrefDist * 1.5f ) vMoveDir = vMoveDir + vFwd;
		else if( fDist < m_fPrefDist * 0.6f ) vMoveDir = vMoveDir - vFwd;
		bHaveMove = true;

		// Don't strafe into walls: check a few times a second (not every frame,
		// which made bots twitch between two walls).
		if( fNow >= m_fNextStrafeProbe && m_fStrafeDir != 0.0f )
		{
			m_fNextStrafeProbe = fNow + 0.3f;
			for( int nTry = 0; nTry < 2; nTry++ )
			{
				IntersectQuery iq;
				IntersectInfo ii;
				iq.m_From = vFeet + LTVector( 0.0f, 30.0f, 0.0f );
				iq.m_To = iq.m_From + vRight * ( m_fStrafeDir * 80.0f );
				iq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
				iq.m_FilterFn = ViewBlockerFn;
				if( !g_pLTServer->IntersectSegment( &iq, &ii )) break;
				if( nTry == 0 )
					m_fStrafeDir = -m_fStrafeDir;
				else
					m_fStrafeDir = 0.0f;		// Walls both sides: just stand and fight.
				m_fNextStrafeChange = fNow + 1.0f;
			}
			vMoveDir = vRight * m_fStrafeDir;
			if( fDist > m_fPrefDist * 1.5f ) vMoveDir = vMoveDir + vFwd;
			else if( fDist < m_fPrefDist * 0.6f ) vMoveDir = vMoveDir - vFwd;
		}

		// Hurt badly: back off toward health while still shooting.
		if( m_bRetreating )
		{
			if( !m_hItemGoal )
			{
				m_fNextItemCheck = 0.0f;
				SeekItems( pBot, vFeet, true );
			}
			LTVector vPathDir;
			if( m_hItemGoal && FollowPath( vFeet, &vPathDir ))
				vMoveDir = vPathDir + vRight * ( m_fStrafeDir * 0.3f );
			else
				vMoveDir = vRight * m_fStrafeDir - vFwd;		// Nowhere to heal: back away.
		}

		// Pinned against something we can see through (railing, glass): route around it.
		bool bKeepPath = false;
		if( !m_bRetreating && fNow < m_fCombatPathUntil )
		{
			bKeepPath = true;
			if( m_aPath.empty( ) && fNow >= m_fNextRepathTime )
			{
				m_fNextRepathTime = fNow + 1.0f;
				PlanPathTo( vFeet, vAimPoint );
			}
			LTVector vPathDir2;
			if( FollowPath( vFeet, &vPathDir2 ))
				vMoveDir = vPathDir2;
			else
				vMoveDir = vRight * m_fStrafeDir;	// No route: hold and strafe.
		}

		if( m_nSkill >= 3 && fNow >= m_fNextJumpTime && pBot->IsBotOnGround( ))
		{
			if( GetRandom( 0, 100 ) < 25 ) input.bJump = true;
			m_fNextJumpTime = fNow + RandF( 1.5f, 4.0f );
		}

		m_fNoPathSince = 0.0f;

		// Our nav path is stale after a fight (unless it leads to health).
		if( !m_bRetreating && !bKeepPath )
			m_aPath.clear( );
	}
	else
	{
		// ---- Hunt / objectives ----
		ChooseWeapon( pBot, -1.0f );

		// Reload during quiet moments, like players do.
		CWeapon* pW = pBot->BotGetCurrentWeapon( );
		if( pW && pW->GetWeaponData( ) && pW->GetWeaponData( )->nShotsPerClip > 0 &&
			pW->GetAmmoInClip( ) < pW->GetWeaponData( )->nShotsPerClip / 2 )
		{
			pBot->BotReload( );
		}

		bool bRecentlyAttacked = m_hLastAttacker && ( fNow - m_fLastAttackedTime < 2.0f );
		bool bHunting = m_hTarget && !bTargetVisible && !m_bRetreating;
		bool bLookAround = false;

		// Heard gunfire nearby: go and see what it is (a player would).
		if( !bHunting && !m_bRetreating )
		{
			LTVector vNoise;
			if( fNow >= m_fNextInvestigate && HearNoise( pBot, &vNoise ) && ( fNow > m_fInvestigateUntil || vNoise.DistSqr( m_vInvestigatePos ) > 400.0f * 400.0f ))
			{
				// Moving toward the sound; feet roughly a body height below the gun.
				if( PlanPathTo( vFeet, vNoise - LTVector( 0.0f, 50.0f, 0.0f )))
				{
					m_vInvestigatePos = vNoise;
					m_fInvestigateUntil = fNow + 10.0f;
					m_fInvestigateArrive = 0.0f;
					m_hItemGoal = LTNULL;
					m_hObjectivePlayer = LTNULL;
				}
				else
					m_fNextInvestigate = fNow + 4.0f;	// Can't get there; don't keep trying.
			}
		}
		bool bInvestigating = !bHunting && fNow < m_fInvestigateUntil;

		if( bHunting )
		{
			// Chase: head for where we last saw them (once per sighting), drop
			// whatever else we were doing.
			m_hItemGoal = LTNULL;
			m_hObjectivePlayer = LTNULL;
			if( m_bHaveLastSeenPos && fNow >= m_fNextRepathTime )
			{
				m_fNextRepathTime = fNow + 0.5f;
				LTVector vDims;
				g_pPhysicsLT->GetObjectDims( m_hTarget, &vDims );
				PlanPathTo( vFeet, m_vTargetLastSeenPos - LTVector( 0.0f, vDims.y, 0.0f ));
				m_bHaveLastSeenPos = false;
			}
		}
		else
		{
			// Objectives.  Keep an item goal tidy (taken / timed out).
			if( m_hItemGoal )
				SeekItems( pBot, vFeet );
			if( m_bRetreating && !m_hItemGoal && fNow >= m_fNextItemCheck )
				SeekItems( pBot, vFeet, true );

			// Going to find someone: re-aim at where they are now and then.
			if( m_hObjectivePlayer )
			{
				if( !IsEnemy( pBot, m_hObjectivePlayer ) || fNow > m_fObjectiveTimeout )
				{
					m_hObjectivePlayer = LTNULL;
					m_aPath.clear( );
				}
				else if( fNow >= m_fObjectiveReplan )
				{
					m_fObjectiveReplan = fNow + 4.0f;
					PlanPathTo( vFeet, FeetOf( m_hObjectivePlayer ));
				}
			}

			bool bNeedGoal = m_aPath.empty( ) || m_nPathIndex >= m_aPath.size( ) || fNow > m_fGoalTimeout;
			if( bInvestigating && !bNeedGoal ) bNeedGoal = false;
			else if( bInvestigating )
			{
				// Got there: look around briefly, then carry on with something else.
				if( m_fInvestigateArrive <= 0.0f ) m_fInvestigateArrive = fNow;
				if( fNow - m_fInvestigateArrive > 2.5f )
				{
					m_fInvestigateUntil = 0.0f;
					m_fInvestigateArrive = 0.0f;
					m_fNextInvestigate = fNow + 8.0f;
					bInvestigating = false;
					bNeedGoal = true;
				}
				else
					bLookAround = true;
			}
			if( bNeedGoal && !bLookAround && !( m_hItemGoal && !m_aPath.empty( )) && fNow >= m_fNextRepathTime )
			{
				m_fNextRepathTime = fNow + 0.5f;
				m_hItemGoal = LTNULL;
				m_hObjectivePlayer = LTNULL;

				// Pick the next thing to do: grab something useful, go and find
				// someone, or just explore.
				float fHealthFrac = pBot->BotGetMaxHealth( ) > 0.0f ? fHealth / pBot->BotGetMaxHealth( ) : 1.0f;
				int nRoll = GetRandom( 0, 99 );
				if( nRoll < (( fHealthFrac < 0.5f ) ? 75 : 45 ))
				{
					m_fNextItemCheck = 0.0f;
					SeekItems( pBot, vFeet );
				}
				if( !m_hItemGoal && nRoll < 45 + ( int )( m_fAggression * 30.0f ))
					PickPlayerObjective( pBot, vFeet );
				if( !m_hItemGoal && !m_hObjectivePlayer )
					PickRoamGoal( vFeet );
			}
		}

		bHaveMove = FollowPath( vFeet, &vMoveDir );

		// ---- Ladders: walk to the foot (or top) of a ladder, climb it, step off. ----
		if( m_iLadder < 0 && m_nPathIndex >= 1 && m_nPathIndex < m_aPath.size( ))
		{
			bool bUpL = false;
			int iL = BotNav::FindLadderLink( m_aPath[m_nPathIndex - 1], m_aPath[m_nPathIndex], &bUpL );
			if( iL >= 0 )
			{
				m_iLadder = iL;
				m_bLadderUp = bUpL;
				m_nLadderPhase = 0;
				m_fLadderStart = fNow;
				g_pLTServer->CPrint( "bot-ladder: %s %s ladder %d from <%.0f,%.0f,%.0f>", pBot->GetNetUniqueName( ), m_bLadderUp ? "climbing" : "descending", m_iLadder, vPos.x, vPos.y, vPos.z );
			}
		}
		if( m_iLadder >= 0 )
		{
			const BotNav::LadderInfo* pL = BotNav::GetLadder( m_iLadder );
			LTVector vToC( 0.0f, 0.0f, 0.0f );
			float fDC = 0.0f;
			if( pL )
			{
				vToC = pL->vClimb - vFeet;
				vToC.y = 0.0f;
				fDC = sqrtf( vToC.x * vToC.x + vToC.z * vToC.z );
			}
			if( !pL || fNow - m_fLadderStart > 25.0f || m_nPathIndex >= m_aPath.size( ) || ( m_nLadderPhase >= 1 && m_nLadderPhase < 2 && fDC > 70.0f ))
			{
				m_iLadder = -1;
				m_aPath.clear( );
				m_fLastProgressTime = fNow;
			}
			else
			{
				LTVector vEnd = BotNav::GetNodePos( m_bLadderUp ? pL->iTop : pL->iBottom );
				bool bLadderFail = false;
				bHaveMove = true;
				if( m_nLadderPhase == 0 )
				{
					if( fDC > 1.0f ) vMoveDir = vToC * ( 1.0f / fDC );
					bool bInside = false;
					if( fDC < 40.0f )
					{
						// Line up on the middle of the ladder so the body is inside the climb volume.
						LTVector vSnap( pL->vClimb.x, vPos.y, pL->vClimb.z );
						g_pLTServer->SetObjectPos( pBot->m_hObject, &vSnap );
						m_nLadderPhase = 1;
						m_fLadderY = vFeet.y;
						m_fLadderYTime = fNow;
					}
					else if( fNow - m_fLadderStart > 7.0f )
						bLadderFail = true;
				}
				if( m_nLadderPhase == 1 )
				{
					input.fClimb = m_bLadderUp ? 1.0f : -1.0f;
					if( fNow - m_fLadderYTime > 1.0f )
					{
						if( fabsf( vFeet.y - m_fLadderY ) < 10.0f )
							bLadderFail = true;
						m_fLadderY = vFeet.y;
						m_fLadderYTime = fNow;
					}
					vMoveDir = ( fDC > 8.0f ) ? vToC * ( 1.0f / fDC ) : LTVector( 0.0f, 0.0f, 0.0f );
					bool bArrived = m_bLadderUp ? ( vFeet.y >= vEnd.y + 6.0f ) : ( vFeet.y <= vEnd.y + 8.0f );
					if( bArrived )
					{
						m_nLadderPhase = 2;
						m_fLadderStart = fNow;	// Time the step-off separately.
					}
				}
				if( bLadderFail )
				{
					g_pLTServer->CPrint( "bot-ladder: %s failed on ladder %d (phase %d) at <%.0f,%.0f,%.0f> centre <%.0f,%.0f,%.0f> half <%.0f,%.0f,%.0f>", pBot->GetNetUniqueName( ), m_iLadder, m_nLadderPhase, vPos.x, vPos.y, vPos.z, pL->vCenter.x, pL->vCenter.y, pL->vCenter.z, pL->vHalf.x, pL->vHalf.y, pL->vHalf.z );
					if( m_nPathIndex >= 1 && m_nPathIndex < m_aPath.size( ))
						BotNav::BlockLink( m_aPath[m_nPathIndex - 1], m_aPath[m_nPathIndex] );
					m_iLadder = -1;
					m_aPath.clear( );
					m_fLastProgressTime = fNow;
					bHaveMove = false;
				}
				else
				if( m_nLadderPhase == 2 )
				{
					LTVector vOff = vEnd - vFeet;
					vOff.y = 0.0f;
					float fOff = sqrtf( vOff.x * vOff.x + vOff.z * vOff.z );
					if( fOff > 1.0f ) vMoveDir = vOff * ( 1.0f / fOff );
					if( m_bLadderUp && pBot->IsBotOnGround( ) == false && fabsf( vFeet.y - vEnd.y ) < 40.0f ) input.bJump = false;
					if(( fOff < 30.0f && fabsf( vFeet.y - vEnd.y ) < 48.0f ) || fNow - m_fLadderStart > 1.5f )
					{
						m_iLadder = -1;
						m_nPathIndex++;			// Past the far end of the ladder.
						m_fLastProgressTime = fNow;
						m_vLastProgressPos = vPos;
					}
				}
			}
		}

		// Off-grid spawn (roof, ledge): no route down, so walk off toward the nearest
		// walkable node like a player would.
		if( fNow < m_fDropUntil )
		{
			LTVector vD = m_vDropGoal - vFeet;
			float fDY = vD.y;
			vD.y = 0.0f;
			float fL = sqrtf( vD.x * vD.x + vD.z * vD.z );
			if( fL < 40.0f || ( fabsf( fDY ) < 30.0f && fL < 120.0f ))
				m_fDropUntil = 0.0f;
			else
			{
				vMoveDir = vD * ( 1.0f / fL );
				bHaveMove = true;
			}
		}

		// Reached the last place we saw them: stand and look around for them.
		if( !bHaveMove && bHunting )
			bLookAround = true;

		// Route finished but the item isn't ours yet: walk straight onto it.
		if( !bHaveMove && m_hItemGoal )
		{
			LTVector vTo = m_vItemPos - vFeet;
			vTo.y = 0.0f;
			float fLen = sqrtf( vTo.x * vTo.x + vTo.z * vTo.z );
			if( fLen < 250.0f && fLen > 12.0f )
			{
				vMoveDir = vTo * ( 1.0f / fLen );
				bHaveMove = true;

				// Close but still not collecting it (on a ledge, behind glass, refused):
				// give up on this item for a while.
				if( m_fNearItemSince <= 0.0f ) m_fNearItemSince = fNow;
				if( fNow - m_fNearItemSince > 2.0f )
				{
					IgnoreItem( m_hItemGoal, 40.0f );
					RememberBadSpot( m_vItemPos );
					m_hItemGoal = LTNULL;
					m_fNearItemSince = 0.0f;
					bHaveMove = false;
				}
			}
			else
			{
				IgnoreItem( m_hItemGoal, 20.0f );
				m_hItemGoal = LTNULL;
				m_fNearItemSince = 0.0f;
			}
		}
		else
			m_fNearItemSince = 0.0f;

		// Nowhere to go for a long time (spawned in a pocket the navigation doesn't connect).
		if( bHaveMove || bLookAround )
			m_fNoPathSince = 0.0f;
		else if( m_fNoPathSince <= 0.0f )
			m_fNoPathSince = fNow;
		else if( fNow - m_fNoPathSince > 2.0f && fNow - m_fNoPathSince <= 6.0f && fNow >= m_fDropUntil )
		{
			int iDrop = BotNav::FindNodeNearHoriz( vFeet, 900.0f, 0, true );
			if( iDrop < 0 ) iDrop = BotNav::FindNearestNode( vFeet, 700.0f );
			if( iDrop >= 0 )
			{
				m_vDropGoal = BotNav::GetNodePos( iDrop );
				if( vFeet.y - m_vDropGoal.y > 150.0f )
					m_fNoPathSince = fNow - 6.5f;	// Tower / ladder-only spawn: too far to fall, move the bot instead.
				else
					m_fDropUntil = fNow + 6.0f;
			}
		}
		else if( fNow - m_fNoPathSince > 6.0f )
		{
			m_fNoPathSince = 0.0f;
			// Sealed in (roof, pocket): move to the nearest node that really connects
			// to the rest of the map rather than dying.
			bool bRescued = false;
			for( int nSkip = 0; nSkip < 10 && !bRescued; nSkip++ )
			{
				int iCand = BotNav::FindNodeNearHoriz( vFeet, 1500.0f, nSkip, false );
				if( iCand < 0 ) break;
				std::vector<int> aTest;
				bool bOk = false;
				for( int nTry = 0; nTry < 3 && !bOk; nTry++ )
					bOk = BotNav::FindPath( iCand, BotNav::RandomNode( ), aTest ) && aTest.size( ) > 20;
				if( !bOk ) continue;
				LTVector vTo = BotNav::GetNodePos( iCand );
				float fHalf = pBot->BotStandHalfHeight( );
				if( fHalf <= 0.0f ) fHalf = 54.0f;
				vTo.y += fHalf + 2.0f;
				g_pLTServer->SetObjectPos( pBot->m_hObject, &vTo );
				g_pLTServer->CPrint( "bot-rescue: %s moved from <%.0f,%.0f,%.0f> to node %d", pBot->GetNetUniqueName( ), vPos.x, vPos.y, vPos.z, iCand );
				m_aPath.clear( );
				m_fLastProgressTime = fNow;
				m_vLastProgressPos = vTo;
				m_fNextRepathTime = 0.0f;
				bRescued = true;
			}
			if( !bRescued )
			{
				g_pLTServer->CPrint( "bot-suicide: %s no route for 6 s at <%.0f,%.0f,%.0f> nodes %u t=%.0f", pBot->GetNetUniqueName( ), vPos.x, vPos.y, vPos.z, ( uint32 )BotNav::GetNumNodes( ), fNow );
				if( fNow > 20.0f )
					KillSelf( pBot );
			}
			return;
		}

		// No route: only walk to the closest nav node if it is right here and in
		// plain view; otherwise pick somewhere else instead of pushing into walls.
		if( !bHaveMove && !bLookAround )
		{
			int iNear = BotNav::FindNearestNode( vFeet, 600.0f );
			if( iNear >= 0 )
			{
				LTVector vNode = BotNav::GetNodePos( iNear );
				IntersectQuery iq;
				IntersectInfo ii;
				iq.m_From = vFeet + LTVector( 0.0f, 30.0f, 0.0f );
				iq.m_To = vNode + LTVector( 0.0f, 30.0f, 0.0f );
				iq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
				iq.m_FilterFn = ViewBlockerFn;
				LTVector vTo = vNode - vFeet;
				vTo.y = 0.0f;
				float fLen = sqrtf( vTo.x * vTo.x + vTo.z * vTo.z );
				if( fLen > 24.0f && !g_pLTServer->IntersectSegment( &iq, &ii ))
				{
					vMoveDir = vTo * ( 1.0f / fLen );
					bHaveMove = true;
				}
			}
			if( !bHaveMove )
			{
				m_hItemGoal = LTNULL;
				m_hObjectivePlayer = LTNULL;
				if( fNow >= m_fNextRepathTime )
				{
					m_fNextRepathTime = fNow + 0.5f;
					PickRoamGoal( vFeet );
				}
			}
		}

		if( bHaveMove && m_iLadder < 0 )
			AvoidWalls( vFeet, &vMoveDir );

		// A closed door in our way: open it, like a player pressing Activate.
		if( bHaveMove && m_iLadder < 0 && fNow >= m_fNextDoorTime )
		{
			m_fNextDoorTime = fNow + 0.5f;
			LTVector vDD( vMoveDir.x, 0.0f, vMoveDir.z );
			float fDL = sqrtf( vDD.x * vDD.x + vDD.z * vDD.z );
			if( fDL > 0.01f )
			{
				vDD = vDD * ( 1.0f / fDL );
				for( int hh = 0; hh < 2; hh++ )
				{
					IntersectQuery dq;
					IntersectInfo di;
					dq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
					dq.m_FilterFn = DoorOnlyFn;
					dq.m_From = LTVector( vFeet.x, vFeet.y + ( hh ? 50.0f : 24.0f ), vFeet.z );
					dq.m_To = dq.m_From + vDD * 72.0f;
					if( g_pLTServer->IntersectSegment( &dq, &di ) && di.m_hObject )
					{
						Door* pDoor = dynamic_cast< Door* >( g_pLTServer->HandleToObject( di.m_hObject ));
						if( pDoor && pDoor->GetState( ) == DOORSTATE_CLOSED )
						{
							// The same door still shut after a few tries (locked, or a window that
							// doesn't open): it is a wall.  Stop routing through it.
							if( di.m_hObject == m_hLastDoor && fNow - m_fLastDoorTry < 3.0f )
								m_nDoorTries++;
							else
								m_nDoorTries = 1;
							m_hLastDoor = di.m_hObject;
							m_fLastDoorTry = fNow;
							if( m_nDoorTries >= 4 )
							{
								m_nDoorTries = 0;
								if( m_nPathIndex >= 1 && m_nPathIndex < m_aPath.size( ))
									BotNav::BlockLink( m_aPath[m_nPathIndex - 1], m_aPath[m_nPathIndex] );
								m_aPath.clear( );
								if( m_hItemGoal )
								{
									LTVector vItD;
									g_pLTServer->GetObjectPos( m_hItemGoal, &vItD );
									IgnoreItem( m_hItemGoal, 60.0f );
									RememberBadSpot( vItD );
									m_hItemGoal = LTNULL;
								}
								m_fNextRepathTime = 0.0f;
								g_pLTServer->CPrint( "bot-door: %s door at <%.0f,%.0f,%.0f> will not open", pBot->GetNetUniqueName( ), vPos.x, vPos.y, vPos.z );
								break;
							}
							HSTRING hAct = g_pLTServer->CreateString( "ACTIVATE" );
							if( hAct )
							{
								SendTriggerMsgToObject( pBot, di.m_hObject, hAct );
								g_pLTServer->FreeString( hAct );
							}
							break;
						}
					}
				}
			}
		}

		// Low doorway or vent ahead (head would hit, crouched height is clear): duck.
		if( bHaveMove && m_iLadder < 0 && fNow >= m_fNextDuckProbe )
		{
			m_fNextDuckProbe = fNow + 0.1f;
			float fStand = pBot->BotStandHalfHeight( );
			if( fStand > 0.0f )
			{
				LTVector vD( vMoveDir.x, 0.0f, vMoveDir.z );
				float fL = sqrtf( vD.x * vD.x + vD.z * vD.z );
				if( fL > 0.01f )
				{
					vD = vD * ( 1.0f / fL );
					float fFeetY = vFeet.y;
					IntersectQuery iq;
					IntersectInfo ii;
					iq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
					iq.m_FilterFn = SightFilterFn;

					iq.m_From = LTVector( vFeet.x, fFeetY + fStand * 2.0f - 6.0f, vFeet.z );
					iq.m_To = iq.m_From + vD * 70.0f;
					// Low obstacle ahead (crate, counter, high step; too tall to step up, short enough
					// to hop): jump it rather than pushing into it.
					if( pBot->IsBotOnGround( ) && fNow >= m_fNextJumpTime )
					{
						IntersectQuery lq;
						IntersectInfo li;
						lq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
						lq.m_FilterFn = ViewBlockerFn;
						lq.m_From = LTVector( vFeet.x, fFeetY + 28.0f, vFeet.z );
						lq.m_To = lq.m_From + vD * 45.0f;
						if( g_pLTServer->IntersectSegment( &lq, &li ))
						{
							lq.m_From = LTVector( vFeet.x, fFeetY + fStand * 0.95f, vFeet.z );
							lq.m_To = lq.m_From + vD * 45.0f;
							if( !g_pLTServer->IntersectSegment( &lq, &li ))
							{
								input.bJump = true;
								m_fNextJumpTime = fNow + 0.8f;
							}
						}
					}

					// Never duck on stairs or ramps: the floor ahead is higher/lower than here.
						bool bOnSlope = false;
						{
							IntersectQuery fq;
							IntersectInfo fi;
							fq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
							fq.m_FilterFn = SightFilterFn;
							fq.m_From = LTVector( vFeet.x + vD.x * 60.0f, fFeetY + 50.0f, vFeet.z + vD.z * 60.0f );
							fq.m_To = fq.m_From - LTVector( 0.0f, 150.0f, 0.0f );
							if( g_pLTServer->IntersectSegment( &fq, &fi ) && fabsf( fi.m_Point.y - fFeetY ) > 14.0f )
								bOnSlope = true;
						}
						bool bHeadBlocked = !bOnSlope && !!g_pLTServer->IntersectSegment( &iq, &ii );
					if( bHeadBlocked )
					{
						iq.m_From = LTVector( vFeet.x, fFeetY + fStand * 1.2f - 4.0f, vFeet.z );
						iq.m_To = iq.m_From + vD * 70.0f;
						if( !g_pLTServer->IntersectSegment( &iq, &ii ))
							{
								// Only a real crawl-through: the way at foot level must be open and
								// there must be floor to walk on.  A window (open at chest height, a
								// wall or counter below) is not a way through.
								IntersectQuery cq;
								IntersectInfo ci;
								cq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
								cq.m_FilterFn = ViewBlockerFn;
								cq.m_From = LTVector( vFeet.x, fFeetY + 10.0f, vFeet.z );
								cq.m_To = cq.m_From + vD * 80.0f;
								bool bFootOpen = !g_pLTServer->IntersectSegment( &cq, &ci );
								cq.m_From = LTVector( vFeet.x + vD.x * 50.0f, fFeetY + 20.0f, vFeet.z + vD.z * 50.0f );
								cq.m_To = cq.m_From - LTVector( 0.0f, 50.0f, 0.0f );
								bool bFloor = !!g_pLTServer->IntersectSegment( &cq, &ci );
								if( bFootOpen && bFloor )
									m_fDuckUntil = fNow + 0.6f;
							}
					}
				}
			}
		}

		// Where we look.  A player looks where they're going, glances toward
		// where an enemy just vanished or toward whoever shot them, and looks
		// around when they've lost someone.
		if( bRecentlyAttacked )
		{
			LTVector vAttacker;
			g_pLTServer->GetObjectPos( m_hLastAttacker, &vAttacker );
			float fWantYaw, fWantPitch;
			DirToAngles( vAttacker - vEye, &fWantYaw, &fWantPitch );
			TurnTowards( fWantYaw, fWantPitch, fFrameTime, sk.fTurnRate );
		}
		else if( bHunting && fNow - m_fTargetLastSeenTime < 1.5f )
		{
			float fWantYaw, fWantPitch;
			DirToAngles( m_vTargetLastSeenPos - vEye, &fWantYaw, &fWantPitch );
			TurnTowards( fWantYaw, fWantPitch, fFrameTime, sk.fTurnRate );
		}
		else if( bInvestigating && !bLookAround && fNow < m_fInvestigateUntil - 9.0f )
		{
			float fWantYaw, fWantPitch;
			DirToAngles( m_vInvestigatePos - vEye, &fWantYaw, &fWantPitch );
			TurnTowards( fWantYaw, 0.0f, fFrameTime, sk.fTurnRate );
		}
		else if( bLookAround )
		{
			if( fNow >= m_fNextLookTime )
			{
				m_fLookYaw = WrapAngle( m_fYaw + RandF( 1.2f, 2.6f ) * (( GetRandom( 0, 1 ) == 0 ) ? -1.0f : 1.0f ));
				m_fNextLookTime = fNow + RandF( 0.8f, 1.6f );
			}
			TurnTowards( m_fLookYaw, 0.0f, fFrameTime, sk.fTurnRate * 0.6f );
		}
		else if( bHaveMove )
		{
			// Only re-face for real changes of direction, not every little correction.
			float fWant = atan2f( vMoveDir.x, vMoveDir.z );
			if( fabs( WrapAngle( fWant - m_fYaw )) > 0.25f || fabs( m_fPitch ) > 0.05f )
				TurnTowards( fWant, 0.0f, fFrameTime, sk.fTurnRate * 0.8f );
		}
	}

	// ---- Stuck detection ----
	// Standing still on purpose (looking around, waiting) is not being stuck: restart the clock.
	if( !bHaveMove )
	{
		m_fLastProgressTime = fNow;
		m_vLastProgressPos = vPos;
	}
	// Walking back out the way we came: after getting stuck, retrace our own trail a few
	// steps, then plan again from there.
	// Wall-following: trapped against walls, so feel along them (like a hand on the wall)
	// until a way out turns up.
	if( m_fWallUntil > 0.0f )
	{
		bool bWallEnd = ( fNow >= m_fWallUntil || m_iLadder >= 0 );
		if( !bWallEnd )
		{
			if( fNow >= m_fWallNext )
			{
				m_fWallNext = fNow + 0.1f;
				LTVector vW = vPos;
				LTVector vFt = vFeet + LTVector( 0.0f, 14.0f, 0.0f );
				bool bFwd = WallProbe( vW, m_vWallDir, 46.0f, pBot->m_hObject ) || WallProbe( vFt, m_vWallDir, 46.0f, pBot->m_hObject );
				if( bFwd )
					m_vWallDir = RotY( m_vWallDir, -m_fWallSide * 0.6f );
				else
				{
					LTVector vSideDir = RotY( m_vWallDir, m_fWallSide * 1.2f );
					if( !WallProbe( vW, vSideDir, 54.0f, pBot->m_hObject ) && !WallProbe( vFt, vSideDir, 54.0f, pBot->m_hObject ))
						m_vWallDir = RotY( m_vWallDir, m_fWallSide * 0.35f );	// Lost the wall: curl back toward it.
				}
			}
			if( fNow - m_fWallProgT > 1.2f )
			{
				if( vPos.DistSqr( m_vWallProgPos ) < 15.0f * 15.0f )
				{
					m_fWallSide = -m_fWallSide;				// Jammed: go round the other way.
					m_vWallDir = RotY( m_vWallDir, kPi );
				}
				m_vWallProgPos = vPos;
				m_fWallProgT = fNow;
			}
			float fWX = vPos.x - m_vWallStart.x, fWZ = vPos.z - m_vWallStart.z;
			if( fWX * fWX + fWZ * fWZ > 300.0f * 300.0f )
				bWallEnd = true;							// Well clear of where we were trapped.
			vMoveDir = m_vWallDir;
			bHaveMove = true;
		}
		if( bWallEnd )
		{
			m_fWallUntil = 0.0f;
			m_aPath.clear( );
			m_fNextRepathTime = 0.0f;
			m_fLastProgressTime = fNow;
			m_vLastProgressPos = vPos;
		}
	}
	if( fNow < m_fBackUntil && m_fWallUntil == 0.0f )
	{
		if( m_iLadder >= 0 || m_nBackIdx >= m_aBack.size( ))
			m_fBackUntil = 0.0f;
		else
		{
			LTVector vT = m_aBack[m_nBackIdx];
			float fBX = vT.x - vPos.x, fBZ = vT.z - vPos.z;
			float fBD = sqrtf( fBX * fBX + fBZ * fBZ );
			if( fBD < 30.0f )
				m_nBackIdx++;
			else
			{
				vMoveDir = LTVector( fBX / fBD, 0.0f, fBZ / fBD );
				bHaveMove = true;
				if( vPos.DistSqr( m_vBackLastPos ) > 20.0f * 20.0f )
				{
					m_vBackLastPos = vPos;
					m_fBackProg = fNow;
				}
				else if( fNow - m_fBackProg > 1.2f )
					m_fBackUntil = 0.0f;
			}
			if( m_nBackIdx >= m_aBack.size( ))
				m_fBackUntil = 0.0f;
		}
		if( m_fBackUntil == 0.0f )
		{
			m_aPath.clear( );
			m_fNextRepathTime = 0.0f;
			m_fLastProgressTime = fNow;
			m_vLastProgressPos = vPos;
		}
	}
	if( bHaveMove && m_iLadder < 0 && fNow >= m_fBackUntil && m_fWallUntil == 0.0f )
	{
		if( vPos.DistSqr( m_vLastProgressPos ) > 48.0f * 48.0f )
		{
			m_vLastProgressPos = vPos;
			m_fLastProgressTime = fNow;
			m_aTrail.push_back( vPos );
			if( m_aTrail.size( ) > 10 )
				m_aTrail.erase( m_aTrail.begin( ));
		}
		else if( fNow - m_fLastProgressTime > 1.0f && pBot->IsBotOnGround( ) && fNow >= m_fNextJumpTime )
		{
			if( GetRandom( 0, 1 ) == 0 )
				input.bJump = true;		// Try hopping over whatever is in the way.
			else
				m_fDuckUntil = fNow + 1.2f;	// ...or ducking under it.
			m_fNextJumpTime = fNow + 1.0f;
			if( bTargetVisible ) m_fCombatPathUntil = fNow + 4.0f;

			// ...and sidestep or back off for a moment.
			float fA = RandF( 1.3f, 2.6f ) * (( GetRandom( 0, 1 ) == 0 ) ? -1.0f : 1.0f );
			float fC = cosf( fA ), fS = sinf( fA );
			m_vUnstuckDir = LTVector( vMoveDir.x * fC + vMoveDir.z * fS, 0.0f, -vMoveDir.x * fS + vMoveDir.z * fC );
			m_fUnstuckUntil = fNow + 0.7f;
		}
		if( fNow < m_fUnstuckUntil && ( m_vUnstuckDir.x != 0.0f || m_vUnstuckDir.z != 0.0f ))
			vMoveDir = m_vUnstuckDir;
		if( fNow - m_fLastProgressTime > 2.5f )
		{
			// Leave a trail in the console / log so stuck spots can be found and fixed.
			// What is in front of us at foot, waist and head height (-1 = clear for 80 units)?
			float fRay[3] = { -1.0f, -1.0f, -1.0f };
			{
				float fSt = pBot->BotStandHalfHeight( );
				if( fSt <= 0.0f ) fSt = 50.0f;
				float fH[3] = { 18.0f, fSt, fSt * 1.9f };
				LTVector vF( vMoveDir.x, 0.0f, vMoveDir.z );
				float fFL = sqrtf( vF.x * vF.x + vF.z * vF.z );
				if( fFL > 0.01f )
				{
					vF = vF * ( 1.0f / fFL );
					for( int r = 0; r < 3; r++ )
					{
						IntersectQuery rq;
						IntersectInfo ri;
						rq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
						rq.m_FilterFn = ViewBlockerFn;
						rq.m_From = LTVector( vFeet.x, vFeet.y + fH[r], vFeet.z );
						rq.m_To = rq.m_From + vF * 80.0f;
						if( g_pLTServer->IntersectSegment( &rq, &ri ))
							fRay[r] = ( ri.m_Point - rq.m_From ).Mag( );
					}
				}
			}
			g_pLTServer->CPrint( "bot-stuck: rays foot/waist/head %.0f/%.0f/%.0f ground %d", fRay[0], fRay[1], fRay[2], pBot->IsBotOnGround( ) ? 1 : 0 );

			g_pLTServer->CPrint( "bot-stuck: %s at <%.0f,%.0f,%.0f> goalnode %d item %d target %d duck %d path %u/%u", pBot->GetNetUniqueName( ), vPos.x, vPos.y, vPos.z, m_iGoalNode, m_hItemGoal ? 1 : 0, m_hTarget ? 1 : 0, ( fNow < m_fDuckUntil ) ? 1 : 0, ( uint32 )m_nPathIndex, ( uint32 )m_aPath.size( ));
			// What is touching us?  8 directions at waist height, anything solid
			// (props, bodies, other characters): distance and kind of the hit.
			{
				float fW = pBot->BotStandHalfHeight( );
				if( fW <= 0.0f ) fW = 50.0f;
				char szDirs[400];
				szDirs[0] = 0;
				for( int d = 0; d < 8; d++ )
				{
					float fAng = d * ( kPi / 4.0f );
					IntersectQuery wq;
					IntersectInfo wi;
					wq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
					wq.m_FilterFn = AnySolidButSelfFn;
					wq.m_pUserData = ( void* )pBot->m_hObject;
					wq.m_From = LTVector( vFeet.x, vFeet.y + fW, vFeet.z );
					wq.m_To = wq.m_From + LTVector( sinf( fAng ), 0.0f, cosf( fAng )) * 48.0f;
					char szOne[40];
					if( g_pLTServer->IntersectSegment( &wq, &wi ))
					{
						int nKind = IsMainWorld( wi.m_hObject ) ? 0 : ( OT_WORLDMODEL == GetObjectType( wi.m_hObject ) ? 1 : ( IsCharacter( wi.m_hObject ) ? 3 : 2 ));
						sprintf( szOne, " %d:%.0f/%c", d, ( wi.m_Point - wq.m_From ).Mag( ), "WBOC"[nKind] );
					}
					else
						sprintf( szOne, " %d:-", d );
					strcat( szDirs, szOne );
				}
				g_pLTServer->CPrint( "bot-stuck: around (W=world B=brush O=model C=char)%s moveDir <%.2f,%.2f>", szDirs, vMoveDir.x, vMoveDir.z );
			}
			// Anything at all (even "non-solid") within 60 units: class, name, flags.
			{
				float fW2 = pBot->BotStandHalfHeight( );
				if( fW2 <= 0.0f ) fW2 = 50.0f;
				for( int d = 0; d < 8; d++ )
				{
					float fAng = d * ( kPi / 4.0f );
					IntersectQuery aq;
					IntersectInfo ai;
					aq.m_Flags = INTERSECT_OBJECTS;
					aq.m_FilterFn = AnySolidButSelfFn;
					aq.m_pUserData = ( void* )pBot->m_hObject;
					aq.m_From = LTVector( vFeet.x, vFeet.y + fW2, vFeet.z );
					aq.m_To = aq.m_From + LTVector( sinf( fAng ), 0.0f, cosf( fAng )) * 60.0f;
					if( g_pLTServer->IntersectSegment( &aq, &ai ) && ai.m_hObject && !IsMainWorld( ai.m_hObject ))
					{
						char szCls[64], szNm[64];
						szCls[0] = 0; szNm[0] = 0;
						HCLASS hCls = g_pLTServer->GetObjectClass( ai.m_hObject );
						if( hCls ) g_pLTServer->GetClassName( hCls, szCls, 64 );
						g_pLTServer->GetObjectName( ai.m_hObject, szNm, 64 );
						uint32 dwFl = 0;
						g_pCommonLT->GetObjectFlags( ai.m_hObject, OFT_Flags, dwFl );
						g_pLTServer->CPrint( "bot-stuck: dir %d hit %s '%s' flags %x at %.0f", d, szCls, szNm, dwFl, ( ai.m_Point - aq.m_From ).Mag( ));
					}
				}
			}
			// Couldn't reach the item we were after (on a shelf, behind furniture...): leave it alone for a while.
			if( m_hItemGoal )
			{
				LTVector vIt;
				g_pLTServer->GetObjectPos( m_hItemGoal, &vIt );
				IgnoreItem( m_hItemGoal, 60.0f );
				RememberBadSpot( vIt );
			}
			if( m_nPathIndex == 0 && !m_aPath.empty( ) && fNow >= m_fDropUntil )
			{
				m_vDropGoal = BotNav::GetNodePos( m_aPath[0] );
				m_fDropUntil = fNow + 6.0f;
			}
			if( m_nPathIndex >= 1 && m_nPathIndex < m_aPath.size( ))
				BotNav::BlockLink( m_aPath[m_nPathIndex - 1], m_aPath[m_nPathIndex] );	// Nobody can cross this link.
			m_aPath.clear( );		// Give up on this route.
			RememberBadSpot( m_iGoalNode >= 0 ? BotNav::GetNodePos( m_iGoalNode ) : vFeet );
			m_hItemGoal = LTNULL;
			m_fNextRepathTime = 0.0f;
			m_fLastProgressTime = fNow;
			m_vLastProgressPos = vPos;
			if( !bTargetVisible )
				PickRoamGoal( vFeet );

			// Walk back out the way we came.
			if( m_aTrail.size( ) >= 2 && fNow - m_fBackLast > 6.0f )
			{
				m_aBack.clear( );
				for( int t = ( int )m_aTrail.size( ) - 1; t >= 0 && m_aBack.size( ) < 4; t-- )
				{
					if( m_aTrail[t].DistSqr( vPos ) > 50.0f * 50.0f )
						m_aBack.push_back( m_aTrail[t] );
				}
				m_aTrail.clear( );
				if( !m_aBack.empty( ))
				{
					m_nBackIdx = 0;
					m_fBackUntil = fNow + 5.0f;
					m_fBackLast = fNow;
					m_fBackProg = fNow;
					m_vBackLastPos = vPos;
				}
			}

			// Repeatedly trapped: do what a player would, and /kill to respawn.
			if( fNow - m_fStuckWindowStart > 20.0f )
			{
				m_fStuckWindowStart = fNow;
				m_nStuckCount = 0;
			}
			++m_nStuckCount;
			if( m_nStuckCount == 2 && m_fWallUntil == 0.0f )
			{
				// Second time: feel around the walls for a way out.
				LTVector vDir( vMoveDir.x, 0.0f, vMoveDir.z );
				float fDL = sqrtf( vDir.x * vDir.x + vDir.z * vDir.z );
				if( fDL < 0.01f ) { vDir = LTVector( 0.0f, 0.0f, 1.0f ); fDL = 1.0f; }
				m_vWallDir = vDir * ( 1.0f / fDL );
				LTVector vWC = vPos;
				bool bR = WallProbe( vWC, RotY( m_vWallDir, 1.5708f ), 80.0f, pBot->m_hObject );
				bool bL = WallProbe( vWC, RotY( m_vWallDir, -1.5708f ), 80.0f, pBot->m_hObject );
				m_fWallSide = ( bR || !bL ) ? 1.0f : -1.0f;
				m_fWallUntil = fNow + 9.0f;
				m_fWallNext = 0.0f;
				m_fWallProgT = fNow;
				m_vWallProgPos = vPos;
				m_vWallStart = vPos;
				m_fBackUntil = 0.0f;
				g_pLTServer->CPrint( "bot-wall: %s following walls from <%.0f,%.0f,%.0f>", pBot->GetNetUniqueName( ), vPos.x, vPos.y, vPos.z );
			}
			else if( m_nStuckCount >= 3 )
			{
				m_nStuckCount = 0;
				// Trapped (pocket the map won't let us leave): move to somewhere that works.
				if( RescueTeleport( pBot, vPos, 400.0f ))
					return;
				g_pLTServer->CPrint( "bot-suicide: %s stuck 3 times at <%.0f,%.0f,%.0f> goalnode %d t=%.0f", pBot->GetNetUniqueName( ), vPos.x, vPos.y, vPos.z, m_iGoalNode, fNow );
				if( fNow > 20.0f )
					KillSelf( pBot );
				return;
			}
		}
	}
	else
	{
		m_vLastProgressPos = vPos;
		m_fLastProgressTime = fNow;
	}

	{
		float fBlend = fFrameTime * ( bTargetVisible ? 9.0f : 18.0f );
		if( fBlend > 1.0f ) fBlend = 1.0f;
		m_vSmoothMove = m_vSmoothMove + ( vMoveDir - m_vSmoothMove ) * fBlend;
		float fM = sqrtf( m_vSmoothMove.x * m_vSmoothMove.x + m_vSmoothMove.z * m_vSmoothMove.z );
		if( vMoveDir.x == 0.0f && vMoveDir.z == 0.0f && fM < 0.15f ) m_vSmoothMove = LTVector( 0.0f, 0.0f, 0.0f );
		input.vMoveDir = ( fM > 0.15f ) ? m_vSmoothMove : vMoveDir;
	}
	input.bDuck = ( fNow < m_fDuckUntil );
	input.fYaw = m_fYaw;
	input.fPitch = m_fPitch;
	pBot->SetBotInput( input );
}
