#ifndef __WINSYNC_H__
#define __WINSYNC_H__
#include <windows.h>

// Minimal critical section wrapper (header missing from the CJ SDK).
class CWinSync_CS
{
public:
	CWinSync_CS()  { InitializeCriticalSection( &m_cs ); }
	~CWinSync_CS() { DeleteCriticalSection( &m_cs ); }
	void Enter() { EnterCriticalSection( &m_cs ); }
	void Leave() { LeaveCriticalSection( &m_cs ); }
private:
	CWinSync_CS( const CWinSync_CS& );
	CWinSync_CS& operator=( const CWinSync_CS& );
	CRITICAL_SECTION m_cs;
};

class CWinSync_CSAuto
{
public:
	CWinSync_CSAuto( CWinSync_CS& cs ) : m_cs( cs ) { m_cs.Enter(); }
	~CWinSync_CSAuto() { m_cs.Leave(); }
private:
	CWinSync_CSAuto& operator=( const CWinSync_CSAuto& );
	CWinSync_CS& m_cs;
};
#endif
