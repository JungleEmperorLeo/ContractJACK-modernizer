param([string]$Cj,[string]$Mod,[string]$Out)
$enc = [Text.Encoding]::GetEncoding(1252)
$cj  = [IO.File]::ReadAllText($Cj,$enc)
$mod = [IO.File]::ReadAllText($Mod,$enc)
function Get-Section([string]$text,[string]$name){
  $m=[regex]::Match($text,'(?ms)^\['+[regex]::Escape($name)+'\].*?(?=^\[|\z)')
  if($m.Success){ return $m.Value } else { return $null }
}
$added=@()
foreach($n in 'ScreenJukebox','ScreenExperimental'){
  if(-not (Get-Section $cj $n)){
    $s = Get-Section $mod $n
    if($s){ $cj += "`r`n" + ($s.TrimEnd() -replace "`r?`n","`r`n") + "`r`n"; $added += $n }
  }
}
# The Display screen has extra Modernizer options; use the taller frame from the mod's layout.
$modDisp = Get-Section $mod 'ScreenDisplay'
$cjDisp  = Get-Section $cj 'ScreenDisplay'
if($modDisp -and $cjDisp){
  $cj = $cj.Replace($cjDisp, (($modDisp.TrimEnd() -replace "`r?`n","`r`n") + "`r`n`r`n"))
  $added += 'ScreenDisplay(replaced)'
}
[IO.File]::WriteAllText($Out,$cj,$enc)
Write-Host ("        Layout.txt: added sections: " + ($added -join ', '))
