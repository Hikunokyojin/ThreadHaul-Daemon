@echo off
rem Builds everything into build\ : the daemon, controller, both simulators
rem and the unit tests. Works from any prompt; finds Visual Studio itself.
rem   build.cmd          build all
rem   build.cmd test     build all, then run the unit tests
setlocal
set "ROOT=%~dp0"
set "SRC=%ROOT%Source"
set "THIRD=%ROOT%third_party"
set "OUT=%ROOT%build"
set "OBJ=%OUT%\obj"

where cl >nul 2>nul
if errorlevel 1 call :find_vs || exit /b 1
if not exist "%OBJ%" mkdir "%OBJ%"

set CFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX
set INC=/I"%SRC%\Actuator" /I"%SRC%\Config" /I"%SRC%\Logging" /I"%SRC%\Telemetry" /I"%SRC%\Daemon" /I"%THIRD%"
set LIBS=advapi32.lib ws2_32.lib winhttp.lib wtsapi32.lib bcrypt.lib user32.lib shell32.lib

set ACTUATOR="%SRC%\Actuator\StateMachine.cpp" "%SRC%\Actuator\ActuatorManager.cpp" "%SRC%\Actuator\PriorityControl.cpp" "%SRC%\Actuator\ThreadControl.cpp"
set TELEMETRY="%SRC%\Telemetry\CpuMath.cpp" "%SRC%\Telemetry\ProcessMonitor.cpp" "%SRC%\Telemetry\AdaptiveThresholds.cpp" "%SRC%\Telemetry\ProcessTable.cpp" "%SRC%\Telemetry\ProcessSafety.cpp"
set CONFIGLOG="%SRC%\Config\Config.cpp" "%SRC%\Logging\JsonLinesLogger.cpp" "%SRC%\Logging\EventLogWriter.cpp"
set DAEMON="%SRC%\Daemon\Daemon.cpp" "%SRC%\Daemon\Alerts.cpp" "%SRC%\Daemon\ConfigSecurity.cpp" "%SRC%\Daemon\ControlPipe.cpp" "%SRC%\Daemon\Coordination.cpp" "%SRC%\Daemon\HttpServer.cpp" "%SRC%\Daemon\Dashboard.cpp"

echo [1/5] ThreadHaulDaemon.exe
cl %CFLAGS% %INC% "%SRC%\main.cpp" %ACTUATOR% %TELEMETRY% %CONFIGLOG% %DAEMON% %LIBS% /Fo"%OBJ%\\" /Fe"%OUT%\ThreadHaulDaemon.exe" >"%OUT%\build.log" || goto :failed
echo [2/5] ThreadHaulController.exe
cl %CFLAGS% %INC% "%SRC%\Controller\controller.cpp" ws2_32.lib /Fo"%OBJ%\\" /Fe"%OUT%\ThreadHaulController.exe" >>"%OUT%\build.log" || goto :failed
echo [3/5] critical_simulator.exe, batch_simulator.exe
cl %CFLAGS% "%SRC%\Simulators\critical_simulator.cpp" /Fo"%OBJ%\\" /Fe"%OUT%\critical_simulator.exe" >>"%OUT%\build.log" || goto :failed
cl %CFLAGS% "%SRC%\Simulators\batch_simulator.cpp" /Fo"%OBJ%\\" /Fe"%OUT%\batch_simulator.exe" >>"%OUT%\build.log" || goto :failed
echo [4/5] unit tests
cl %CFLAGS% %INC% "%SRC%\Tests\StateMachineTests.cpp" %ACTUATOR% advapi32.lib /Fo"%OBJ%\\" /Fe"%OUT%\StateMachineTests.exe" >>"%OUT%\build.log" || goto :failed
cl %CFLAGS% %INC% "%SRC%\Tests\ConfigLoggingTests.cpp" "%SRC%\Config\Config.cpp" "%SRC%\Logging\JsonLinesLogger.cpp" /Fo"%OBJ%\\" /Fe"%OUT%\ConfigLoggingTests.exe" >>"%OUT%\build.log" || goto :failed
cl %CFLAGS% %INC% "%SRC%\Tests\TelemetryTests.cpp" %TELEMETRY% advapi32.lib /Fo"%OBJ%\\" /Fe"%OUT%\TelemetryTests.exe" >>"%OUT%\build.log" || goto :failed
echo [5/5] sample config and dashboard files
copy /y "%SRC%\Config\config.json" "%OUT%\config.json" >nul
if not exist "%OUT%\dashboard" mkdir "%OUT%\dashboard"
copy /y "%SRC%\Dashboard\*" "%OUT%\dashboard\" >nul
echo Build OK -^> %OUT%

if /i "%~1"=="test" goto :test
exit /b 0

:test
set FAILED=0
for %%t in (StateMachineTests TelemetryTests ConfigLoggingTests) do (
    echo [%%t]
    "%OUT%\%%t.exe" || set FAILED=1
)
exit /b %FAILED%

:failed
type "%OUT%\build.log"
echo BUILD FAILED
exit /b 1

:find_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio not found. Install "Desktop development with C++".
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo Visual Studio C++ tools not found.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b 0
