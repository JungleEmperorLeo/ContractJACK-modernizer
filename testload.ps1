$game = Split-Path -Parent $PSScriptRoot
$dll  = Join-Path $PSScriptRoot 'src\Game\built\Final\ClientShellDll\TO2\CShell.dll'
$env:PATH = "$game;$env:PATH"
Set-Location $game
Write-Host ("PowerShell is " + ([IntPtr]::Size*8) + "-bit")
Add-Type -Namespace N -Name W -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32", SetLastError=true, CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr LoadLibraryW(string p);
'@
foreach ($d in @("$game\SDL2.dll", "$game\GameSpy.dll", $dll)) {
  $h = [N.W]::LoadLibraryW($d)
  if ($h -eq [IntPtr]::Zero) {
    $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Host ("FAILED  " + $d + "  error " + $e + ": " + (New-Object ComponentModel.Win32Exception $e).Message)
  } else { Write-Host ("OK      " + $d) }
}
