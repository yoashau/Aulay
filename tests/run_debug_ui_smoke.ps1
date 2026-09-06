param([string]$ExePath, [string]$Profile)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new()
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class AuditWindow {
 public delegate bool Callback(IntPtr window, IntPtr param);
 [DllImport("user32.dll")] public static extern bool EnumWindows(Callback cb, IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint pid);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint msg, IntPtr wp, IntPtr lp);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w, uint msg, IntPtr wp, IntPtr lp);
}
'@
function Find-Window([int]$ProcId, [string]$Class) {
 $script:found=[IntPtr]::Zero
 [AuditWindow]::EnumWindows({param($w,$unused)
  $owner=[uint32]0; [AuditWindow]::GetWindowThreadProcessId($w,[ref]$owner)|Out-Null
  if ($owner -eq $ProcId) {
   $s=New-Object Text.StringBuilder 128
   [AuditWindow]::GetClassName($w,$s,128)|Out-Null
   if($s.ToString() -eq $Class){$script:found=$w; return $false}
  }
  return $true
 },[IntPtr]::Zero)|Out-Null
 return $script:found
}

$config=Join-Path (Split-Path $ExePath) 'Aulay.json'
[IO.File]::WriteAllText($config,'{"reconnect":false,"lastDevices":[]}',[Text.UTF8Encoding]::new($false))
$p=Start-Process -FilePath $ExePath -PassThru
try {
 $w=[IntPtr]::Zero
 for($i=0;$i -lt 150 -and $w -eq [IntPtr]::Zero;$i++) {
  Start-Sleep -Milliseconds 40
  if($p.HasExited){throw 'Process exited during initialization'}
  $w=Find-Window $p.Id 'Aulay'
 }
 if($w -eq [IntPtr]::Zero){throw 'Host window missing'}
 Start-Sleep -Milliseconds 800
 [AuditWindow]::SendMessage($w,0x8001,[IntPtr]::Zero,[IntPtr]0x0400)|Out-Null
 if(-not $p.WaitForExit(15000)){throw 'UI fixture did not finish'}
 $log=Join-Path $env:LOCALAPPDATA ("Aulay\Logs\Aulay-"+$p.Id+".log")
 $text=Get-Content $log -Raw -Encoding UTF8
 if($text -notmatch 'UI_FIXTURE_RESULT=PASS') {Write-Output $text;throw 'UI fixture failed'}
 if($text -notmatch 'attempt=4242[^\r\n]*USER_NO_SOUND marker=1[^\r\n]*previous-disconnect-attempt=4241'){throw 'Marker chain missing'}
 $audioFiles=Get-ChildItem (Split-Path $log) -Filter ("Aulay-"+$p.Id+"-*-audio*.log")
 $audio=($audioFiles | ForEach-Object {Get-Content $_.FullName -Raw -Encoding UTF8}) -join "`n"
 if($audio -notmatch 'marker=2 reason=USER_NO_SOUND[^\r\n]*capture-end'){throw 'Second audio snapshot missing'}
 if($audio -notmatch 'monitor-stop queue-drained=true'){throw 'Monitor not drained'}
 $bundles=Get-ChildItem (Split-Path $log) -Filter ("Aulay-"+$p.Id+"-*-silent-*.log")
 if($bundles.Count -ne 2){throw 'Two marker bundles expected'}
 foreach($bundle in $bundles){
  $body=Get-Content $bundle.FullName -Raw -Encoding UTF8
  if($body -notmatch 'USER_NO_SOUND' -or $body -notmatch 'bundle-end'){throw 'Marker bundle incomplete'}
 }
 "UI_AUDIO_BUNDLES=2 DRAIN=PASS"
 if($text -match 'transaction-start'){throw 'UI marker started radio recovery'}
 $text -split '\r?\n' | Where-Object {$_ -match '\[ui-test\]'} | Write-Output
 if($p.ExitCode -ne 0){throw "Unexpected exit code $($p.ExitCode)"}
 "UI_FIXTURE_EXIT=0 LOG=$log"
} finally {
 if(-not $p.HasExited){Stop-Process -Id $p.Id -Force;$p.WaitForExit()}
}
