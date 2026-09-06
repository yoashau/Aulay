@echo off
setlocal
pushd "%~dp0.." || exit /b 1
set "TEST_OUTPUT=%TEMP%\Aulay-debug-monitor-%RANDOM%-%RANDOM%"
mkdir "%TEST_OUTPUT%"
cl /nologo /std:c++20 /EHsc /MD /utf-8 /I"packages\Microsoft.Windows.ImplementationLibrary.1.0.200519.2\include" tests\windows_debug_monitor.cpp /Fo:"%TEST_OUTPUT%\monitor.obj" /Fe:"%TEST_OUTPUT%\monitor.exe" WindowsApp.lib ole32.lib advapi32.lib
if not "%ERRORLEVEL%"=="0" goto failed
"%TEST_OUTPUT%\monitor.exe" %*
if not "%ERRORLEVEL%"=="0" goto failed
rmdir /s /q "%TEST_OUTPUT%"
popd
exit /b 0
:failed
rmdir /s /q "%TEST_OUTPUT%"
popd
exit /b 1
