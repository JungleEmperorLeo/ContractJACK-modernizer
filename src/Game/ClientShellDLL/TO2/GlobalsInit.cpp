#include "stdafx.h"
#include "TO2GameClientShell.h"

// These macros create a bunch of static variables whose constructors do initialization
// for hooking into the engine.  They need to get created before all other static variables
// in the project.  

ILTModelClient*		g_pILTModelClient = NULL;
define_holder(ILTModelClient, g_pILTModelClient);

ILTDrawPrim*		g_pDrawPrim;
define_holder(ILTDrawPrim,g_pDrawPrim);

ILTFontManager*		g_pFontManager;
define_holder(ILTFontManager,g_pFontManager);

ILTTexInterface*	g_pTexInterface;		
define_holder(ILTTexInterface,g_pTexInterface);

SETUP_CLIENTSHELL();

define_interface(CTO2GameClientShell, IClientShell);


// Contract J.A.C.K.'s engine only accepts game DLLs that export this marker
// (the retail CShell.dll and Object.lto both export it, value 0). It is not in the SDK source.
__declspec(dllexport) int _0x33560e700xc1c20x4dcc0xa20x60x450x4c0x1a0x560x3e0x3d = 0;



