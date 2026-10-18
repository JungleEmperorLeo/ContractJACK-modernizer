// ----------------------------------------------------------------------- //
//
// MODULE  : BotPlayer.cpp
//
// PURPOSE : Server-side bot support for CPlayerObj.  A bot is an ordinary
//           player object without a client.  Everything a client would
//           normally send (movement, aim, firing, reloads, respawn requests)
//           is produced here from the input the bot brain sets each frame,
//           using the same player code paths so bots obey the same rules.
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "PlayerObj.h"
#include "BotBrain.h"
#include "Attachments.h"
#include "Weapon.h"
#include "WeaponMgr.h"
#include "GameServerShell.h"
#include "ServerMissionMgr.h"
#include "ServerUtilities.h"
#include "CommonUtilities.h"
#include "SharedMovement.h"
#include "MsgIDs.h"
#include <math.h>

namespace
{
	const float kStepHeight		= 24.0f;	// Highest ledge a bot walks up without jumping.
	const float kGroundProbe	= 4.0f;		// How far below the feet still counts as standing.
	const float kSnapDown		= 20.0f;	// Follow the floor down slopes/stairs by this much.

	bool BotMoveFilterFn( HOBJECT hObj, void* /*pUserData*/ )
	{
		return ( IsMainWorld( hObj ) || OT_WORLDMODEL == GetObjectType( hObj ));
	}

	// Things that stop a walking bot: world, brushes and solid props.
	bool BotSlideFilterFn( HOBJECT hObj, void* /*pUserData*/ )
	{
		if( IsMainWorld( hObj ) || OT_WORLDMODEL == GetObjectType( hObj )) return true;
		return ( OT_MODEL == GetObjectType( hObj ) && IsKindOf( hObj, "Prop" )) ? true : false;
	}

	// Finds the floor below vPos (object centre).  Returns the floor height.
	bool FindFloor( const LTVector& vPos, float fHalfHeight, float fBelow, float* pY, LTVector* pNormal )
	{
		// Centre plus four points near the edge of the body, so standing on a
		// ledge edge or curb still counts as standing.  Keep the highest hit.
		static const float kOff[5][2] = { { 0.0f, 0.0f }, { 14.0f, 0.0f }, { -14.0f, 0.0f }, { 0.0f, 14.0f }, { 0.0f, -14.0f } };
		bool bHit = false;
		float fBest = 0.0f;
		LTVector vBestN( 0.0f, 1.0f, 0.0f );
		for( int i = 0; i < 5; i++ )
		{
			IntersectQuery iq;
			IntersectInfo ii;
			iq.m_From = vPos + LTVector( kOff[i][0], 0.0f, kOff[i][1] );
			iq.m_To = iq.m_From - LTVector( 0.0f, fHalfHeight + fBelow, 0.0f );
			iq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID | INTERSECT_HPOLY;
			iq.m_FilterFn = BotMoveFilterFn;
			if( !g_pLTServer->IntersectSegment( &iq, &ii ))
				continue;
			if( !bHit || ii.m_Point.y > fBest )
			{
				bHit = true;
				fBest = ii.m_Point.y;
				vBestN = ii.m_Plane.Normal( );
			}
			if( i == 0 && vBestN.y > 0.45f ) break;	// Centre is fine: cheap common case.
		}
		if( !bHit ) return false;
		*pY = fBest;
		if( pNormal ) *pNormal = vBestN;
		return true;
	}

	float Now( ) { return g_pLTServer->GetTime( ); }
}

// ----------------------------------------------------------------------- //

uint32 CPlayerObj::GetPlayerClientID( ) const
{
	if( m_bIsBot )
		return m_nBotClientID;
	return m_hClient ? g_pLTServer->GetClientID( m_hClient ) : ( uint32 )-1;
}

void CPlayerObj::InitBot( uint32 nBotID, const char* szName, CBotBrain* pBrain )
{
	m_bIsBot = true;
	m_nBotClientID = nBotID;
	m_sBotName = szName ? szName : "Bot";
	if( m_pBotBrain && m_pBotBrain != pBrain )
		delete m_pBotBrain;
	m_pBotBrain = pBrain;
	m_bAllowInput = LTTRUE;
	m_fBotVelY = 0.0f;
	m_bBotOnGround = false;
	m_fBotStandHalf = 0.0f;
	m_bBotDucked = false;
	m_fBotDeathTime = 0.0f;
	m_fBotRespawnDelay = 2.0f + ( float )GetRandom( 0, 20 ) * 0.1f;
}

CWeapon* CPlayerObj::BotGetCurrentWeapon( )
{
	return m_pPlayerAttachments ? m_pPlayerAttachments->GetWeapon( ) : LTNULL;
}

CWeapon* CPlayerObj::BotGetWeapon( uint8 nWeaponId )
{
	return m_pPlayerAttachments ? m_pPlayerAttachments->GetWeapon( nWeaponId ) : LTNULL;
}

int CPlayerObj::BotGetAmmoCount( int nAmmoId )
{
	return m_pPlayerAttachments ? m_pPlayerAttachments->GetAmmoCount( nAmmoId ) : 0;
}

LTVector CPlayerObj::BotGetEyePos( )
{
	LTVector vPos;
	g_pLTServer->GetObjectPos( m_hObject, &vPos );
	LTVector vEye = vPos + GetHeadOffset( );
	if( m_bBotDucked && m_fBotStandHalf > 0.0f )
		vEye.y -= m_fBotStandHalf * 0.4f;	// Crouching lowers the eyes.
	return vEye;
}

// ----------------------------------------------------------------------- //
// Weapons
// ----------------------------------------------------------------------- //

static void SendBotWeaponSound( CPlayerObj* pPlayer, uint8 nType, uint8 nWeaponId, const LTVector& vPos );

bool CPlayerObj::BotSelectWeapon( uint8 nWeaponId )
{
	if( !m_pPlayerAttachments || m_damage.IsDead( ))
		return false;

	CWeapon* pNew = m_pPlayerAttachments->GetWeapon( nWeaponId );
	if( !pNew || !pNew->Have( ))
		return false;

	CWeapon* pCur = m_pPlayerAttachments->GetWeapon( );
	if( pCur && pCur->GetId( ) == nWeaponId )
		return true;

	// Pick ammo the weapon can use that we actually have.
	WEAPON const* pData = g_pWeaponMgr->GetWeapon( nWeaponId );
	if( !pData ) return false;
	int nAmmoId = pData->nDefaultAmmoId;
	if( BotGetAmmoCount( nAmmoId ) <= 0 )
	{
		for( int i = 0; i < pData->nNumAmmoIds; i++ )
		{
			if( BotGetAmmoCount( pData->aAmmoIds[i] ) > 0 )
			{
				nAmmoId = pData->aAmmoIds[i];
				break;
			}
		}
	}

	DoWeaponChange( nWeaponId, ( uint8 )nAmmoId );
	m_nWeaponStatus = WS_SELECT;
	m_fBotReloadDoneTime = 0.0f;
	m_fBotWeaponSwitchDoneTime = Now( ) + 0.6f;	// Same feel as a player's select animation.

	LTVector vPos;
	g_pLTServer->GetObjectPos( m_hObject, &vPos );
	SendBotWeaponSound( this, PSI_SELECT, nWeaponId, vPos );
	return true;
}

bool CPlayerObj::BotReload( )
{
	CWeapon* pWeapon = BotGetCurrentWeapon( );
	if( !pWeapon || BotIsReloading( )) return false;

	WEAPON const* pData = pWeapon->GetWeaponData( );
	if( !pData || pData->nShotsPerClip <= 0 ) return false;
	if( pWeapon->GetAmmoInClip( ) >= pData->nShotsPerClip ) return false;

	int nAmmo = BotGetAmmoCount( pWeapon->GetAmmoId( ));
	if( nAmmo <= pWeapon->GetAmmoInClip( )) return false;	// Nothing more to load.

	m_nWeaponStatus = WS_RELOADING;
	m_fBotReloadDoneTime = Now( ) + 1.6f;

	LTVector vPos;
	g_pLTServer->GetObjectPos( m_hObject, &vPos );
	SendBotWeaponSound( this, PSI_RELOAD, ( uint8 )pWeapon->GetId( ), vPos );
	return true;
}

static void SendBotWeaponSound( CPlayerObj* pPlayer, uint8 nType, uint8 nWeaponId, const LTVector& vPos )
{
	// Same message a client sends for its weapon sounds; the server relays it to everyone.
	CAutoMessage cMsg;
	cMsg.Writeuint8( nType );
	cMsg.Writeuint8( nWeaponId );
	cMsg.Writeuint8( 0 );
	cMsg.WriteLTVector( vPos );
	pPlayer->HandleWeaponSoundMessage( cMsg.Read( ));
}

void CPlayerObj::UpdateBotWeapon( )
{
	float fNow = Now( );

	if( m_fBotWeaponSwitchDoneTime > 0.0f && fNow >= m_fBotWeaponSwitchDoneTime )
	{
		m_fBotWeaponSwitchDoneTime = 0.0f;
		if( m_nWeaponStatus == WS_SELECT )
			m_nWeaponStatus = WS_NONE;
	}

	if( m_fBotReloadDoneTime > 0.0f && fNow >= m_fBotReloadDoneTime )
	{
		m_fBotReloadDoneTime = 0.0f;
		CWeapon* pWeapon = BotGetCurrentWeapon( );
		if( pWeapon )
			DoWeaponReload(( uint8 )pWeapon->GetAmmoId( ));
		m_nWeaponStatus = WS_NONE;
	}
}

// Builds the same MID_WEAPON_FIRE payload a client sends and feeds it to the
// normal handler, so damage, projectiles, effects and ammo all work as usual.
bool CPlayerObj::BotFireWeapon( )
{
	float fNow = Now( );
	if( fNow < m_fBotNextFireTime || BotIsReloading( ) || m_fBotWeaponSwitchDoneTime > 0.0f )
		return false;

	CWeapon* pWeapon = BotGetCurrentWeapon( );
	if( !pWeapon ) return false;
	WEAPON const* pWeaponData = pWeapon->GetWeaponData( );
	AMMO const* pAmmoData = pWeapon->GetAmmoData( );
	if( !pWeaponData || !pAmmoData ) return false;

	// Out of bullets in the clip?  Reload like a player would.
	if( pWeaponData->nShotsPerClip > 0 && pWeapon->GetAmmoInClip( ) <= 0 )
	{
		BotReload( );
		return false;
	}
	if( !pWeaponData->bInfiniteAmmo && BotGetAmmoCount( pWeapon->GetAmmoId( )) <= 0 )
		return false;

	LTRotation rAim( m_BotInput.fPitch, m_BotInput.fYaw, 0.0f );
	LTVector vDir = rAim.Forward( );
	LTVector vEye = BotGetEyePos( );
	LTVector vFlash = HandHeldWeaponFirePos( pWeapon );

	// Moving spreads the shots more, as it does for players.
	bool bMoving = ( m_BotInput.vMoveDir.x * m_BotInput.vMoveDir.x + m_BotInput.vMoveDir.z * m_BotInput.vMoveDir.z ) > 0.01f;
	uint8 nPerturb = bMoving ? 160 : 40;
	if( m_pBotBrain )
	{
		float fP = ( float )nPerturb * m_pBotBrain->GetPerturbScale( );
		nPerturb = ( uint8 )(( fP > 255.0f ) ? 255.0f : fP );
	}

	CAutoMessage cMsg;
	cMsg.Writeuint8( 0 );								// fire type
	cMsg.Writeuint8(( uint8 )pWeapon->GetId( ));
	cMsg.Writeuint8(( uint8 )pWeapon->GetAmmoId( ));
	cMsg.WriteLTVector( vFlash );
	cMsg.WriteLTVector( vEye );
	cMsg.WriteLTVector( vDir );
	cMsg.Writeuint8(( uint8 )GetRandom( 0, 255 ));		// random seed
	cMsg.Writeuint8( nPerturb );
	cMsg.Writeuint32(( uint32 )( fNow * 1000.0f ));
	cMsg.Writebool( false );							// no client-side object impact
	HandleWeaponFireMessage( cMsg.Read( ));

	SendBotWeaponSound( this, PSI_FIRE, ( uint8 )pWeapon->GetId( ), vFlash );

	// Rate of fire.  Players are limited by the weapon animations; approximate
	// that with the weapon's fire delay and a human-like click rate for
	// small-clip (semi automatic) weapons.
	float fDelay = ( float )pWeaponData->m_nFireDelay / 1000.0f;
	if( fDelay < 0.1f ) fDelay = 0.1f;
	if( pWeaponData->nShotsPerClip > 0 && pWeaponData->nShotsPerClip <= 8 )
		fDelay += 0.25f;
	if( m_pBotBrain ) fDelay *= m_pBotBrain->GetFireDelayScale( );
	m_fBotNextFireTime = fNow + fDelay;

	// Auto-reload when the clip runs dry.
	if( pWeaponData->nShotsPerClip > 0 && pWeapon->GetAmmoInClip( ) <= 0 )
		BotReload( );

	return true;
}

// ----------------------------------------------------------------------- //
// Movement: a server-side version of the client's player movement.
// ----------------------------------------------------------------------- //

void CPlayerObj::UpdateBotMovement( float fFrameTime )
{
	if( fFrameTime <= 0.0f ) return;
	if( fFrameTime > 0.1f ) fFrameTime = 0.1f;

	LTVector vPos, vDims;
	g_pLTServer->GetObjectPos( m_hObject, &vPos );
	g_pPhysicsLT->GetObjectDims( m_hObject, &vDims );

	// Ducking really shrinks the collision box (low doorways, vents), like a
	// player's crouch.  Standing back up needs headroom.
	if( m_fBotStandHalf <= 0.0f && !m_bBotDucked )
		m_fBotStandHalf = vDims.y;
	if( m_fBotStandHalf > 0.0f && m_BotInput.bDuck != m_bBotDucked )
	{
		const float kCrouchFrac = 0.6f;
		float fStand = m_fBotStandHalf, fCrouch = fStand * kCrouchFrac;
		if( m_BotInput.bDuck )
		{
			LTVector vNewDims( vDims.x, fCrouch, vDims.z );
			g_pPhysicsLT->SetObjectDims( m_hObject, &vNewDims, 0 );
			vPos.y -= ( fStand - fCrouch );
			g_pLTServer->SetObjectPos( m_hObject, &vPos );
			m_bBotDucked = true;
			vDims = vNewDims;
		}
		else
		{
			// Headroom check: from the top of the crouched box up to standing height.
			IntersectQuery iq;
			IntersectInfo ii;
			float fFeetY = vPos.y - fCrouch;
			iq.m_From = LTVector( vPos.x, fFeetY + fCrouch * 2.0f, vPos.z );
			iq.m_To = LTVector( vPos.x, fFeetY + fStand * 2.0f + 2.0f, vPos.z );
			iq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
			iq.m_FilterFn = BotMoveFilterFn;
			if( !g_pLTServer->IntersectSegment( &iq, &ii ))
			{
				LTVector vNewDims( vDims.x, fStand, vDims.z );
				vPos.y += ( fStand - fCrouch );
				g_pLTServer->SetObjectPos( m_hObject, &vPos );
				g_pPhysicsLT->SetObjectDims( m_hObject, &vNewDims, 0 );
				m_bBotDucked = false;
				vDims = vNewDims;
			}
			else
				m_BotInput.bDuck = true;		// No room: stay down.
		}
	}

	LTVector vGravity;
	g_pPhysicsLT->GetGlobalForce( vGravity );
	if( vGravity.y > -100.0f ) vGravity.y = -2000.0f;

	// Facing.
	LTRotation rRot( 0.0f, m_BotInput.fYaw, 0.0f );
	g_pLTServer->SetObjectRotation( m_hObject, &rRot );
	// Pitch is sent to clients with the object's special fx, so throttle it.
	// SetPitch takes radians (the engine compresses +-90 degrees into a byte).
	if( fabs( m_BotInput.fPitch - m_fPitch ) > 0.03f && Now( ) >= m_fBotNextPitchTime )
	{
		SetPitch( m_BotInput.fPitch );
		m_fBotNextPitchTime = Now( ) + 0.1f;
	}

	// Ground check.
	float fFloorY = 0.0f;
	LTVector vNormal( 0.0f, 1.0f, 0.0f );
	bool bWasOnGround = m_bBotOnGround;
	bool bFloor = FindFloor( vPos, vDims.y, kGroundProbe, &fFloorY, &vNormal );
	// Not level geometry: could still be standing on a prop (table, crate) or
	// wedged on something solid.  Nudge down; if we can't move, we're supported.
	bool bSupported = false;
	if( !bFloor && m_fBotVelY <= 0.0f )
	{
		LTVector vDown = vPos - LTVector( 0.0f, 3.0f, 0.0f );
		g_pLTServer->MoveObject( m_hObject, &vDown );
		LTVector vChk;
		g_pLTServer->GetObjectPos( m_hObject, &vChk );
		if( vChk.y > vPos.y - 1.5f ) bSupported = true;
		vPos = vChk;
	}
	m_bBotOnGround = ( bFloor && m_fBotVelY <= 0.0f && vNormal.y > 0.45f ) || bSupported;
	if( m_BotInput.fClimb != 0.0f ) m_bBotOnGround = false;	// On a ladder: no standing, no gravity.

	if( m_bBotOnGround )
	{
		m_fBotVelY = 0.0f;
		if( !bWasOnGround && m_nMotionStatus != MS_NONE )
			m_nMotionStatus = MS_LANDED;

		if( m_BotInput.bJump )
		{
			float fJumpMul = ( m_fJumpMultiplier > 0.0f ) ? m_fJumpMultiplier : 1.0f;
			m_fBotVelY = m_fJumpVel * fJumpMul;
			m_bBotOnGround = false;
			m_nMotionStatus = MS_JUMPED;
		}
	}
	else
	{
		if( m_BotInput.fClimb != 0.0f )
			m_fBotVelY = m_BotInput.fClimb * (( m_fLadderVel > 60.0f ) ? m_fLadderVel : 160.0f );
		else
			m_fBotVelY += vGravity.y * fFrameTime;
		if( m_fBotVelY < -3000.0f ) m_fBotVelY = -3000.0f;
		if( m_nMotionStatus == MS_NONE )
			m_nMotionStatus = MS_FALLING;
	}
	SetOnGround( m_bBotOnGround );

	// Horizontal speed: same values the client uses.
	float fMoveMul = ( m_fMoveMultiplier > 0.0f ) ? m_fMoveMultiplier : 1.0f;
	float fSpeed = ( m_BotInput.bRun ? m_fRunVel : m_fWalkVel ) * fMoveMul;
	if( m_BotInput.bDuck ) fSpeed = m_fWalkVel * fMoveMul * 0.5f;

	LTVector vMove = m_BotInput.vMoveDir;
	vMove.y = 0.0f;
	float fLen = sqrtf( vMove.x * vMove.x + vMove.z * vMove.z );
	if( fLen > 0.001f ) vMove = vMove * ( 1.0f / fLen ); else vMove = LTVector( 0.0f, 0.0f, 0.0f );
	if( !m_bBotOnGround ) fSpeed *= 0.8f;	// Little air control.

	LTVector vHoriz = vMove * ( fSpeed * fFrameTime );

	// Don't walk off cliffs: a player sees the edge and stops.  Drops a player
	// would take on purpose (a few storeys at most) are still allowed.
	if( m_bBotOnGround && m_BotInput.fClimb == 0.0f && ( vMove.x != 0.0f || vMove.z != 0.0f ))
	{
		LTVector vAhead( vPos.x + vMove.x * 32.0f, vPos.y, vPos.z + vMove.z * 32.0f );
		float fAheadFloor;
		if( !FindFloor( vAhead, vDims.y, 260.0f, &fAheadFloor, LTNULL ))
			vHoriz = LTVector( 0.0f, 0.0f, 0.0f );
	}

	// Horizontal move with step-up for stairs.
	if( vHoriz.x * vHoriz.x + vHoriz.z * vHoriz.z > 0.0001f )
	{
		LTVector vStart = vPos;
		LTVector vDest = vPos + vHoriz;
		// Follow rising ground (terrain, ramps) instead of sliding along it flat.
		if( m_bBotOnGround )
		{
			float fAheadY = 0.0f;
			LTVector vAheadN( 0.0f, 1.0f, 0.0f );
			LTVector vProbe( vDest.x, vPos.y + kStepHeight, vDest.z );
			if( FindFloor( vProbe, vDims.y, kStepHeight + kSnapDown, &fAheadY, &vAheadN ) && vAheadN.y > 0.45f )
			{
				float fRise = fAheadY - ( vPos.y - vDims.y );
				if( fRise > 0.5f && fRise <= kStepHeight )
					vDest.y += fRise;
			}
		}
		g_pLTServer->MoveObject( m_hObject, &vDest );
		LTVector vAfter;
		g_pLTServer->GetObjectPos( m_hObject, &vAfter );

		float fWanted = vHoriz.x * vHoriz.x + vHoriz.z * vHoriz.z;
		float fGot = ( vAfter.x - vStart.x ) * ( vAfter.x - vStart.x ) + ( vAfter.z - vStart.z ) * ( vAfter.z - vStart.z );
		if( fGot < fWanted * 0.6f && ( m_bBotOnGround || m_fBotVelY <= 0.0f ))
		{
			// Blocked: try again from a step higher (stairs, small ledges).
			g_pLTServer->SetObjectPos( m_hObject, &vStart );
			LTVector vUp = vStart + LTVector( 0.0f, kStepHeight, 0.0f );
			g_pLTServer->MoveObject( m_hObject, &vUp );
			LTVector vRaised;
			g_pLTServer->GetObjectPos( m_hObject, &vRaised );
			LTVector vFwd = vRaised + vHoriz;
			g_pLTServer->MoveObject( m_hObject, &vFwd );
			LTVector vStepped;
			g_pLTServer->GetObjectPos( m_hObject, &vStepped );
			float fGot2 = ( vStepped.x - vStart.x ) * ( vStepped.x - vStart.x ) + ( vStepped.z - vStart.z ) * ( vStepped.z - vStart.z );
			if( fGot2 > fGot + 0.01f )
			{
				// Settle back down onto the step.
				LTVector vDown = vStepped - LTVector( 0.0f, kStepHeight, 0.0f );
				g_pLTServer->MoveObject( m_hObject, &vDown );
			}
			else
			{
				g_pLTServer->SetObjectPos( m_hObject, &vAfter );

				// Slide along what we are pushing against, the way a player does,
				// instead of stopping dead on every wall, corner and doorframe.
				LTVector vSlide( 0.0f, 0.0f, 0.0f );
				{
					float fWLen = sqrtf( fWanted );
					LTVector vDir = vHoriz * ( 1.0f / fWLen );
					IntersectQuery wq;
					IntersectInfo wi;
					wq.m_Flags = INTERSECT_OBJECTS | IGNORE_NONSOLID;
					wq.m_FilterFn = BotSlideFilterFn;
					wq.m_From = vAfter;
					wq.m_To = vAfter + vDir * 40.0f;
					if( g_pLTServer->IntersectSegment( &wq, &wi ))
					{
						LTVector vN = wi.m_Plane.Normal( );
						vN.y = 0.0f;
						float fNL = sqrtf( vN.x * vN.x + vN.z * vN.z );
						if( fNL > 0.1f )
						{
							vN = vN * ( 1.0f / fNL );
							float fD = vHoriz.x * vN.x + vHoriz.z * vN.z;
							if( fD < 0.0f )
							{
								vSlide = vHoriz - vN * fD;
								vSlide.y = 0.0f;
							}
						}
					}
				}
				if( vSlide.x * vSlide.x + vSlide.z * vSlide.z > 0.01f )
				{
					LTVector vSD = vAfter + vSlide;
					g_pLTServer->MoveObject( m_hObject, &vSD );
				}
				else
				{
					// No usable surface normal: try each axis on its own.
					LTVector vC;
					g_pLTServer->GetObjectPos( m_hObject, &vC );
					LTVector vTX = vC + LTVector( vHoriz.x, 0.0f, 0.0f );
					g_pLTServer->MoveObject( m_hObject, &vTX );
					g_pLTServer->GetObjectPos( m_hObject, &vC );
					LTVector vTZ = vC + LTVector( 0.0f, 0.0f, vHoriz.z );
					g_pLTServer->MoveObject( m_hObject, &vTZ );
				}
			}
		}
	}

	// Vertical move.
	g_pLTServer->GetObjectPos( m_hObject, &vPos );
	if( !m_bBotOnGround )
	{
		LTVector vDest = vPos + LTVector( 0.0f, m_fBotVelY * fFrameTime, 0.0f );
		g_pLTServer->MoveObject( m_hObject, &vDest );
		LTVector vAfter;
		g_pLTServer->GetObjectPos( m_hObject, &vAfter );
		// Hit the ceiling while going up?
		if( m_fBotVelY > 0.0f && vAfter.y < vDest.y - 0.5f )
			m_fBotVelY = 0.0f;
	}
	else
	{
		// Stay glued to the floor going down stairs and slopes.
		float fY;
		if( FindFloor( vPos, vDims.y, kSnapDown, &fY, LTNULL ))
		{
			float fTargetY = fY + vDims.y + 0.1f;
			if( fTargetY < vPos.y - 0.2f )
			{
				LTVector vDest( vPos.x, fTargetY, vPos.z );
				g_pLTServer->MoveObject( m_hObject, &vDest );
			}
		}
	}

	// Control flags drive the animator exactly like a client's would.
	uint32 nFlags = 0;
	if( fLen > 0.001f )
	{
		LTVector vForward( sinf( m_BotInput.fYaw ), 0.0f, cosf( m_BotInput.fYaw ));
		LTVector vRight( cosf( m_BotInput.fYaw ), 0.0f, -sinf( m_BotInput.fYaw ));
		float fF = vMove.Dot( vForward );
		float fR = vMove.Dot( vRight );
		if( fF > 0.3f ) nFlags |= BC_CFLG_FORWARD;
		else if( fF < -0.3f ) nFlags |= BC_CFLG_REVERSE;
		if( fR > 0.3f ) nFlags |= BC_CFLG_STRAFE_RIGHT;
		else if( fR < -0.3f ) nFlags |= BC_CFLG_STRAFE_LEFT;
		nFlags |= BC_CFLG_MOVING;
	}
	if( m_BotInput.bRun ) nFlags |= BC_CFLG_RUN;
	if( m_BotInput.bDuck ) nFlags |= BC_CFLG_DUCK;
	if( m_BotInput.bJump ) nFlags |= BC_CFLG_JUMP;
	if( m_BotInput.bFire ) nFlags |= BC_CFLG_FIRING;
	SetControlFlags( nFlags );
}

// ----------------------------------------------------------------------- //

void CPlayerObj::UpdateBot( )
{
	if( !m_bIsBot ) return;

	float fNow = Now( );

	// Dead: wait a moment, then respawn the way a client's respawn request does.
	if( m_damage.IsDead( ) || GetState( ) != PS_ALIVE )
	{
		m_BotInput = BotInput( );
		SetControlFlags( 0 );
		if( m_fBotDeathTime <= 0.0f )
		{
			if( m_bBotDucked && m_fBotStandHalf > 0.0f )
			{
				LTVector vD;
				g_pPhysicsLT->GetObjectDims( m_hObject, &vD );
				vD.y = m_fBotStandHalf;
				g_pPhysicsLT->SetObjectDims( m_hObject, &vD, 0 );
			}
			m_bBotDucked = false;
			m_fBotStandHalf = 0.0f;
			m_fBotDeathTime = fNow;
			if( m_pBotBrain ) m_pBotBrain->OnDeath( this );
		}
		else if( fNow - m_fBotDeathTime > m_fBotRespawnDelay && CanRespawn( ))
		{
			Respawn( );
			ResetAfterDeath( );
			SetClientLoaded( true );
			m_fBotDeathTime = 0.0f;
			m_fBotVelY = 0.0f;
			m_fBotReloadDoneTime = 0.0f;
			m_fBotWeaponSwitchDoneTime = 0.0f;
			m_nWeaponStatus = WS_NONE;
			m_fBotRespawnDelay = 2.0f + ( float )GetRandom( 0, 20 ) * 0.1f;
			if( m_pBotBrain ) m_pBotBrain->OnRespawn( this );
		}
		return;
	}

	float fFrameTime = g_pLTServer->GetFrameTime( );

	if( m_pBotBrain )
		m_pBotBrain->Update( this, fFrameTime );

	UpdateBotWeapon( );

	if( m_BotInput.bFire )
		BotFireWeapon( );

	UpdateBotMovement( fFrameTime );
}
