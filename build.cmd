@echo off
rem ThreadHaul build script (MSVC, no CMake required).
rem   build.cmd          build everything into build\
rem   build.cmd test     build, then run the unit tests
rem Output:
rem   build\bin\     ThreadHaulDaemon.exe, ThreadHaulController.exe, simulators,
rem                  dashboard\ and a config.json for console runs
rem   build\tests\   unit test executables
setlocal
set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "THIRD=%ROOT%third_party"
set "OUT=%ROOT%build"
set "BIN=%OUT%\bin"
set "TST=%OUT%\tests"
set "OBJ=%OUT%\obj"

where cl >nul 2>nul
if errorlevel 1 call :find_vs || exit /b 1
for %%d in ("%BIN%" "%TST%" "%OBJ%" "%BIN%\dashboard") do if not exist %%d mkdir %%d

set CFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX
set INC=/I"%SRC%\actuator" /I"%SRC%\config" /I"%SRC%\logging" /I"%SRC%\telemetry" /I"%SRC%\service" /I"%THIRD%"
set LIBS=advapi32.lib ws2_32.lib winhttp.lib wtsapi32.lib bcrypt.lib user32.lib shell32.lib

set ACTUATOR="%SRC%\actuator\StateMachine.cpp" "%SRC%\actuator\ActuatorManager.cpp" "%SRC%\actuator\PriorityControl.cpp" "%SRC%\actuator\ThreadControl.cpp"
set TELEMETRY="%SRC%\telemetry\CpuMath.cpp" "%SRC%\telemetry\ProcessMonitor.cpp" "%SRC%\telemetry\AdaptiveThresholds.cpp" "%SRC%\telemetry\ProcessTable.cpp" "%SRC%\telemetry\ProcessSafety.cpp"
set CONFIGLOG="%SRC%\config\Config.cpp" "%SRC%\logging\JsonLinesLogger.cpp" "%SRC%\logging\EventLogWriter.cpp"
set SERVICE="%SRC%\service\Daemon.cpp" "%SRC%\service\Alerts.cpp" "%SRC%\service\ConfigSecurity.cpp" "%SRC%\service\ControlPipe.cpp" "%SRC%\service\Coordination.cpp" "%SRC%\service\HttpServer.cpp" "%SRC%\service\Dashboard.cpp" "%SRC%\service\AutoProtect.cpp"

echo [1/5] ThreadHaulDaemon.exe
cl %CFLAGS% %INC% "%SRC%\service\main.cpp" %ACTUATOR% %TELEMETRY% %CONFIGLOG% %SERVICE% %LIBS% /Fo"%OBJ%\\" /Fe"%BIN%\ThreadHaulDaemon.exe" >"%OUT%\build.log" || goto :failed
echo [2/5] ThreadHaulController.exe
cl %CFLAGS% %INC% "%SRC%\controller\main.cpp" ws2_32.lib /Fo"%OBJ%\\" /Fe"%BIN%\ThreadHaulController.exe" >>"%OUT%\build.log" || goto :failed
echo [3/5] simulators
cl %CFLAGS% "%ROOT%tools\simulators\critical_simulator.cpp" /Fo"%OBJ%\\" /Fe"%BIN%\critical_simulator.exe" >>"%OUT%\build.log" || goto :failed
cl %CFLAGS% "%ROOT%tools\simulators\batch_simulator.cpp" /Fo"%OBJ%\\" /Fe"%BIN%\batch_simulator.exe" >>"%OUT%\build.log" || goto :failed
echo [4/5] unit tests
cl %CFLAGS% %INC% "%ROOT%tests\StateMachineTests.cpp" %ACTUATOR% advapi32.lib /Fo"%OBJ%\\" /Fe"%TST%\StateMachineTests.exe" >>"%OUT%\build.log" || goto :failed
cl %CFLAGS% %INC% "%ROOT%tests\ConfigLoggingTests.cpp" "%SRC%\config\Config.cpp" "%SRC%\logging\JsonLinesLogger.cpp" /Fo"%OBJ%\\" /Fe"%TST%\ConfigLoggingTests.exe" >>"%OUT%\build.log" || goto :failed
cl %CFLAGS% %INC% "%ROOT%tests\TelemetryTests.cpp" %TELEMETRY% advapi32.lib /Fo"%OBJ%\\" /Fe"%TST%\TelemetryTests.exe" >>"%OUT%\build.log" || goto :failed
echo [5/5] dashboard and sample config
copy /y "%ROOT%web\dashboard\*" "%BIN%\dashboard\" >nul
copy /y "%ROOT%config\config.example.json" "%BIN%\config.json" >nul
echo Build OK -^> %BIN%

if /i "%~1"=="test" goto :test
exit /b 0

:test
set FAILED=0
for %%t in (StateMachineTests TelemetryTests ConfigLoggingTests) do (
    echo [%%t]
    "%TST%\%%t.exe" || set FAILED=1
)
exit /b %FAILED%

:failed
type "%OUT%\build.log"
echo BUILD FAILED
exit /b 1

:find_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio not found. Install the "Desktop development with C++" workload.
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo Visual Studio C++ tools not found.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b 0
