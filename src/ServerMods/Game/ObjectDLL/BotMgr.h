// ----------------------------------------------------------------------- //
//
// MODULE  : BotMgr.h
//
// PURPOSE : Multiplayer bots - console commands and bot bookkeeping.
//           Bots are CAIHuman characters spawned at multiplayer start points.
//
// ----------------------------------------------------------------------- //

#ifndef __BOT_MGR_H__
#define __BOT_MGR_H__

namespace BotMgr
{
	// Called from CGameServerShell::HandleConsoleCommand.  Returns true if the
	// command was a bot command (so it should not go on to the command mgr).
	// Client usage:  cmd bot-add [model] [goalset] [weapon]
	bool HandleConsoleCommand( HCLIENT hSender, const char* szCommand );

	// Forget all bots (call on level change).
	void Clear( );

	// Called every server frame: keeps the requested number of player bots in the game.
	void Update( );
}

#endif // __BOT_MGR_H__
