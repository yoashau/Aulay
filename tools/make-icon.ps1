# Build application icons with the production DirectWrite tray renderer.
param([string]$Output = '')
$ErrorActionPreference = 'Stop'
if ($env:PATHEXT -notmatch '(^|;)\.EXE(;|$)') { $env:PATHEXT = '.COM;.EXE;.BAT;.CMD;' + $env:PATHEXT }
$root = if ($PSScriptRoot) { Split-Path -Parent $PSScriptRoot } else { (Get-Location).Path }
if (-not $Output) { $Output = Join-Path $root 'Aulay.ico' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = if ($vs) { Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' } else {
    # Also support an existing build-tools installation absent from vswhere's catalog.
    Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat') -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $vcvars) { throw 'Visual Studio C++ build tools are required to run the production icon renderer.' }
$wil = Join-Path $root 'packages\Microsoft.Windows.ImplementationLibrary.1.0.200519.2\include'
if (-not (Test-Path (Join-Path $wil 'wil\common.h'))) { throw 'Restore the solution NuGet packages first.' }
$scratch = Join-Path $env:LOCALAPPDATA ('AulayBuild\icons-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
try {
    Copy-Item (Join-Path $root 'tools\IconGenerator.cpp') $scratch
    Copy-Item (Join-Path $root 'TrayIcon.hpp') $scratch
    $command = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$scratch"
cl.exe /nologo /std:c++20 /EHsc /MD /utf-8 /I"$wil" IconGenerator.cpp /Fe:IconGenerator.exe WindowsApp.lib advapi32.lib user32.lib gdi32.lib
if errorlevel 1 exit /b 1
IconGenerator.exe "$Output"
exit /b %ERRORLEVEL%
"@
    $script = Join-Path $scratch 'generate.cmd'
    [IO.File]::WriteAllText($script, $command, [Text.Encoding]::Default)
    & $env:ComSpec /d /c $script
    if ($LASTEXITCODE -ne 0) { throw 'Application icon generation failed.' }
} finally { Remove-Item -LiteralPath $scratch -Recurse -Force }
