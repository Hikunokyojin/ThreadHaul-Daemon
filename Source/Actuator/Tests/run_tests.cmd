@echo off
rem Builds and runs the Actuator unit tests. Works from any prompt: it finds
rem Visual Studio via vswhere and loads the x64 toolchain itself.
setlocal
set "HERE=%~dp0"
set "SRC=%HERE%.."
set "OUT=%HERE%bin"

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio not found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /W3 /DUNICODE /D_UNICODE /I"%SRC%" ^
   "%HERE%StateMachineTests.cpp" ^
   "%SRC%\StateMachine.cpp" "%SRC%\ActuatorManager.cpp" ^
   "%SRC%\PriorityControl.cpp" "%SRC%\ThreadControl.cpp" ^
   advapi32.lib /Fo"%OUT%\\" /Fe"%OUT%\StateMachineTests.exe" || exit /b 1

"%OUT%\StateMachineTests.exe"
