// ---------------------------------------------------------------------------
//  CJ71Options.h
//
//  Contract J.A.C.K. fork: the client hands the server its start-up options as
//  a byte image laid out the way Visual C++ 7.1 (VS2003, msvcp71) lays out
//  ServerGameOptions -- the same layout the retail client sends.  This server
//  is built with a modern compiler whose std::string / std::set are laid out
//  differently, so it must not treat that image as its own ServerGameOptions.
//  These helpers read the image into a native ServerGameOptions and write the
//  server's start result back into it.
// ---------------------------------------------------------------------------

#ifndef __CJ71OPTIONS_H__
#define __CJ71OPTIONS_H__

#include "ProfileUtils.h"

namespace CJ71
{
	// Fills 'out' from the VC7.1-layout image the client passed to the server.
	void ReadImage( const char* pImage, ServerGameOptions& out );

	// Writes the start result back into the client's image (offset 0xAC).
	void SetStartResult( char* pImage, EServerStartResult eResult );
}

#endif // __CJ71OPTIONS_H__
