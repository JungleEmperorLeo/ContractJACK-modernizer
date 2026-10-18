NOLF2 Modernizer -> Contract J.A.C.K.  (experimental source port)
==================================================================

WHAT THIS IS
  The haekb/nolf2-modernizer source (widescreen, SDL2 input/gamepad, console,
  jukebox code, graphics fixes...) merged onto the Contract J.A.C.K. 1.0 SDK
  source, client side only.  It builds CShell.dll, CRes.dll and ClientFx.fxd
  and installs them as a mod in  Custom\mods\Modernizer.

  *** NOTHING HERE HAS BEEN COMPILED YET. ***  It was merged by script with no
  access to Visual Studio.  Expect compile errors on the first run.  That is
  normal: run build.bat, send me build_errors.log and build.log, and I fix them.

HOW TO BUILD
  1. Install Visual Studio 2019 or 2022 (Community is fine) with
       - workload "Desktop development with C++"
       - component "C++ MFC for latest build tools (x86 and x64)"
  2. Double-click build.bat (leave this folder inside your ConJack folder).
  3. Pick the "Modernizer" mod in the game's Mods menu.

  Options:  build.bat /nolayout   skip the Layout.txt patch (see below)
            build.bat /nobuild    only re-pack / re-install

WHAT IS NOT INCLUDED
  - Lithtech.exe: the original's hex-edited engine is NOT ported (different
    engine build).  The retail Lithtech.exe stays untouched.
  - Server side (Object.lto / server.dll) is unchanged and retail.
  - Modernizer's custom internet server browser (JServerDir): Contract J.A.C.K.'s
    own GameSpy screens (Multi/Join/Host/Main) are used unchanged.
  - Jukebox music list: the Modernizer's Jukebox.txt lists NOLF2 music, so the
    jukebox will be empty.
  - controls.cfg / default profile of the Modernizer are not copied.
  - Source is the SDK v1.0; your game is patched to 1.1 (small changes).

LAYOUT.TXT
  The Jukebox/Experimental option screens need layout entries.  build.bat
  extracts Update_v1x1.rez / Game.rez once (slow, ~1.4 GB of temp files that are
  deleted again) to read the game's Layout.txt, appends the two missing screen
  sections, and puts the patched copy in the mod.  Skip with /nolayout.

UNINSTALL
  Delete  Custom\mods\Modernizer  and  SDL2.dll  from the ConJack folder.

LICENSE
  Bound by the NOLF2 / Contract J.A.C.K. tools EULA (see src\EULA.htm) and the
  open source licences in src\osslicenses.txt.
