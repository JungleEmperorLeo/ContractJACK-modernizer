// ----------------------------------------------------------------------- //
//
// MODULE  : TO2GameClientShell.cpp
//
// PURPOSE : Game Client Shell - Implementation
//
// CREATED : 11/5/01
//
// (c) 1997-2001 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "TO2GameClientShell.h"

#include "msgids.h"

#include "clientmultiplayermgr.h"
#include "IGameSpy.h"

#include "InfoFromSE.h"

VarTrack			g_vtMaxFPS;						// MaxFramerate				<0...>
VarTrack			g_vtLockFPS;					// FramerateLock			<0-1>
VarTrack			g_vtShowFPS;					// ShowFramerate			<0-1>

// Sample rate
int g_nSampleRate = 22050;

// Plain C-style helper (no C++ objects) so __try is allowed.
static void ScanCrashStack(CONTEXT* pCtx, char* szDiag, size_t nCap)
{
	char szTmp[300];
	sprintf(szTmp, "\nEIP %08X ESP %08X EBP %08X EAX %08X ECX %08X EDX %08X", pCtx->Eip, pCtx->Esp, pCtx->Ebp, pCtx->Eax, pCtx->Ecx, pCtx->Edx);
	if (strlen(szDiag) + strlen(szTmp) < nCap - 1) strcat(szDiag, szTmp);
	DWORD* pStk = (DWORD*)pCtx->Esp; int nFound = 0;
	__try
	{
		for (int d = 0; d < 400 && nFound < 14; d++)
		{
			MEMORY_BASIC_INFORMATION mbi; memset(&mbi, 0, sizeof(mbi));
			if (!VirtualQuery((void*)pStk[d], &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE) continue;
			if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
			char szMod[MAX_PATH]; szMod[0] = 0;
			GetModuleFileNameA((HMODULE)mbi.AllocationBase, szMod, MAX_PATH);
			const char* pName = strrchr(szMod, '\\'); pName = pName ? pName + 1 : szMod;
			sprintf(szTmp, "\n S+%X: %s +0x%X", d * 4, pName, (unsigned)(pStk[d] - (DWORD)mbi.AllocationBase));
			if (strlen(szDiag) + strlen(szTmp) < nCap - 1) strcat(szDiag, szTmp);
			nFound++;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

LONG WINAPI UncaughtExceptionHandler(EXCEPTION_POINTERS* ExceptionInfo)
{
	g_pLTClient->CPrint("!! CRASH DETECTED !! Dumping console log to debug.log!");
	g_pLTClient->RunConsoleString("WriteToDebugLog");

	// We've done the essentials, the rest is gravy.

	ShowCursor(true);

	auto info = InfoFromSE::information(ExceptionInfo);

	// Diagnostics: which module / offset faulted, and the call stack.
	{
		char szDiag[2048]; szDiag[0] = 0;
		char szTmp[300];
		void* aStack[24];
		USHORT nFrames = CaptureStackBackTrace(0, 24, aStack, NULL);
		sprintf(szTmp, "\nCode 0x%08X  Addr %p", ExceptionInfo->ExceptionRecord->ExceptionCode, ExceptionInfo->ExceptionRecord->ExceptionAddress);
		strcat(szDiag, szTmp);
		void* aAddr[25]; int nAddr = 0;
		aAddr[nAddr++] = ExceptionInfo->ExceptionRecord->ExceptionAddress;
		for (USHORT f = 0; f < nFrames && nAddr < 25; f++) aAddr[nAddr++] = aStack[f];
		for (int a = 0; a < nAddr; a++)
		{
			MEMORY_BASIC_INFORMATION mbi; memset(&mbi, 0, sizeof(mbi));
			char szMod[MAX_PATH]; szMod[0] = 0;
			if (VirtualQuery(aAddr[a], &mbi, sizeof(mbi)) && mbi.AllocationBase)
			{
				GetModuleFileNameA((HMODULE)mbi.AllocationBase, szMod, MAX_PATH);
				const char* pName = strrchr(szMod, '\\'); pName = pName ? pName + 1 : szMod;
				sprintf(szTmp, "\n%s%s +0x%X  (type %X)", a == 0 ? "AT " : "  ", pName[0] ? pName : "<no module>", (unsigned)((char*)aAddr[a] - (char*)mbi.AllocationBase), (unsigned)mbi.Type);
			}
			else sprintf(szTmp, "\n%s<unmapped> %p", a == 0 ? "AT " : "  ", aAddr[a]);
			if (strlen(szDiag) + strlen(szTmp) < sizeof(szDiag) - 1) strcat(szDiag, szTmp);
		}
		ScanCrashStack(ExceptionInfo->ContextRecord, szDiag, sizeof(szDiag));
		g_pLTClient->CPrint("CRASHDIAG: %s", szDiag);
		info += szDiag;
	}

	info += "\nPlease check debug.log for more information!";

	auto pWindow = SDL_CreateWindow("Error", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 0, 0, 0);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "!! Crash Detected !!", info.c_str(), pWindow);
	SDL_DestroyWindow(pWindow);
	pWindow = nullptr;

	return EXCEPTION_CONTINUE_SEARCH;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CTO2GameClientShell::CTO2GameClientShell()
//
//	PURPOSE:	Initialization
//
// ----------------------------------------------------------------------- //

CTO2GameClientShell::CTO2GameClientShell() : CGameClientShell()
{
	// Just some default
	m_fMaxFPS = 60.0f;
	m_lNextUpdate = 1L;
	m_lFrametime = 0L;
	m_bLockFramerate = LTTRUE;
	m_bUserWantsFramerateLock = LTTRUE;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CTO2GameClientShell::~CTO2GameClientShell()
//
//	PURPOSE:	Destruction
//
// ----------------------------------------------------------------------- //

CTO2GameClientShell::~CTO2GameClientShell()
{
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CTO2GameClientShell::OnEngineInitialized
//
//	PURPOSE:	Wrapper for OnEngineInitialized so we can do something extra
//
// ----------------------------------------------------------------------- //

uint32 CTO2GameClientShell::OnEngineInitialized(RMode *pMode, LTGUID *pAppGuid)
{
	// Set a top level exception handler to catch any bugs and dump our console log.
	// This may or may not work depending on what crashed, but it's worth a try.
	// This won't happen in debug mode!
	SetUnhandledExceptionFilter(UncaughtExceptionHandler);

	uint32 nResult = CGameClientShell::OnEngineInitialized(pMode, pAppGuid);

	m_VersionMgr.Update();

	//with the update of the version manager, it might have changed the gore setting,
	//so make sure that the FX managers know about the change
	UpdateGoreSettings();

	// Setup our framerate limiter
	m_bLockFramerate = LTTRUE;

	g_vtLockFPS.Init(g_pLTClient, "LockFramerate", LTNULL, 1.0f);
	g_vtShowFPS.Init(g_pLTClient, "ShowFramerate", LTNULL, 0.0f);
	g_vtMaxFPS.Init(g_pLTClient, "MaxFramerate", LTNULL, 60.0f);

	// If we can't get timer frequency, we can't limit the framerate!
	if (!QueryPerformanceFrequency(&m_lTimerFrequency)) {
		SDL_Log("Device doesn't support high resolution timer! Can't lock framerate.");
		m_bLockFramerate = LTFALSE;
	}

	// Init with a default!
	m_fMaxFPS = g_vtMaxFPS.GetFloat();
	m_lFrametime = ((LTFLOAT)m_lTimerFrequency.QuadPart / m_fMaxFPS);

	return nResult;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CTO2GameClientShell::ProcessPacket
//
//	PURPOSE:	Handling for connectionless networking packets
//
// ----------------------------------------------------------------------- //

LTRESULT CTO2GameClientShell::ProcessPacket(ILTMessage_Read *pMsg, uint8 senderAddr[4], uint16 senderPort)
{
	// Gotta have some data...
	if (pMsg->Size() < 16)
		return LT_OK;

	// Check if we have a gamespy server browser.
	if( g_pClientMultiplayerMgr->GetRetailServerBrowser( ))
	{
		// Read the data out of the message.
		uint32 nDataLen = pMsg->Size() / 8;
		uint8* pData = ( uint8* )alloca( nDataLen );
		pMsg->ReadData( pData, nDataLen * 8 );

		// See if the gamespybrowser wants it.
		g_pClientMultiplayerMgr->GetRetailServerBrowser( )->HandleNetMessage( pData, nDataLen, senderAddr, senderPort );

		// Reset to beginning for the rest of the function.
		pMsg->Seek( 0 );
	}

	// Skip the engine-side portion of the message header
	CLTMsgRef_Read cSubMsg(pMsg->SubMsg(8));

	// Remember what address this came from
	g_pClientMultiplayerMgr->SetCurMessageSource(senderAddr, senderPort);

	// Hand it off as if it were a message
	// Note : This has to go through the parent's OnMessage so we handle the message source properly
	CGameClientShell::OnMessage(cSubMsg);

	return LT_OK;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CTO2GameClientShell::OnMessage
//
//	PURPOSE:	Message from the server
//
// ----------------------------------------------------------------------- //
void CTO2GameClientShell::OnMessage(ILTMessage_Read *pMsg)
{
	// Store the server's address as where this message came from
	uint8 aAddrBuffer[4];
	uint16 nPort;
	g_pLTClient->GetServerIPAddress(aAddrBuffer, &nPort);
	g_pClientMultiplayerMgr->SetCurMessageSource(aAddrBuffer, nPort);
	
	CGameClientShell::OnMessage(pMsg);
}

void CTO2GameClientShell::LimitFramerate()
{

	// Update the max fps if they changed it!
	if (m_fMaxFPS != g_vtMaxFPS.GetFloat())
	{
		m_fMaxFPS = g_vtMaxFPS.GetFloat();
		m_lFrametime = ((LTFLOAT)m_lTimerFrequency.QuadPart / m_fMaxFPS);
	}

	// Occasionally we'll need to unlock the framerate (like during loading!)
	// But we also want the user to have the option to unlock it,
	// so that's why there's two almost identical lock vars here.
	if ((m_bLockFramerate && g_vtLockFPS.GetFloat()))
	{
		// Limit our framerate so the game actually runs properly.
		LARGE_INTEGER NewTime;

		while (1) {
			QueryPerformanceCounter(&NewTime);
			auto lTime = NewTime.QuadPart - m_lNextUpdate;
			if (lTime > m_lFrametime) {
				m_lNextUpdate = NewTime.QuadPart;
				break;
			}
		}
	}
}

void CTO2GameClientShell::Update()
{
	CGameClientShell::Update();

	CTO2PlayerStats* pPlayerStats = (CTO2PlayerStats*)g_pInterfaceMgr->GetPlayerStats();

	pPlayerStats->UpdateFramerate(1 / GetFrameTime());

	LimitFramerate();
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CTO2GameClientShell::OnEnterWorld()
//
//	PURPOSE:	Handle entering world
//
// ----------------------------------------------------------------------- //

void CTO2GameClientShell::OnEnterWorld()
{
	CGameClientShell::OnEnterWorld();

#ifndef _FINAL
	//override for debugging purposes
	float fVal = 0.0f;
    if (!g_pLTClient) return;

    HCONSOLEVAR hVar = g_pLTClient->GetConsoleVar("LowViolence");
	if(hVar)
	{
        fVal = g_pLTClient->GetVarValueFloat(hVar);
		char buf[128] = "";
		sprintf(buf, "serv LowViolence %f",fVal);
		g_pLTClient->RunConsoleString(buf);
	}


#endif
}