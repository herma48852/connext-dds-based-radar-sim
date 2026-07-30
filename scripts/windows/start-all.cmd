@echo off
setlocal EnableExtensions

rem Native Command Prompt launcher. This file intentionally does not invoke
rem PowerShell. All three child processes inherit the RTI and QoS environment.

for %%I in ("%~dp0..\..") do set "REPO_ROOT=%%~fI"

set "DOMAIN=92"
set "CONTROL_DOMAIN="
set "TARGETS=32"
set "CONFIGURATION=RelWithDebInfo"
set "CONNEXT_DIR="
set "BUILD_DIR="
set "RUN_SECONDS="
set "DISABLE_SUB_3KM="

:parse_arguments
if "%~1"=="" goto arguments_parsed
if /I "%~1"=="-Domain" (
    if "%~2"=="" goto missing_value
    set "DOMAIN=%~2"
    shift
    shift
    goto parse_arguments
)
if /I "%~1"=="-ControlDomain" (
    if "%~2"=="" goto missing_value
    set "CONTROL_DOMAIN=%~2"
    shift
    shift
    goto parse_arguments
)
if /I "%~1"=="-Targets" (
    if "%~2"=="" goto missing_value
    set "TARGETS=%~2"
    shift
    shift
    goto parse_arguments
)
if /I "%~1"=="-Configuration" (
    if "%~2"=="" goto missing_value
    set "CONFIGURATION=%~2"
    shift
    shift
    goto parse_arguments
)
if /I "%~1"=="-ConnextDir" (
    if "%~2"=="" goto missing_value
    set "CONNEXT_DIR=%~2"
    shift
    shift
    goto parse_arguments
)
if /I "%~1"=="-BuildDir" (
    if "%~2"=="" goto missing_value
    set "BUILD_DIR=%~2"
    shift
    shift
    goto parse_arguments
)
if /I "%~1"=="-RunSeconds" (
    if "%~2"=="" goto missing_value
    set "RUN_SECONDS=%~2"
    shift
    shift
    goto parse_arguments
)
if /I "%~1"=="-DisableSub3km" (
    set "DISABLE_SUB_3KM=1"
    shift
    goto parse_arguments
)
if /I "%~1"=="-Help" goto usage
if /I "%~1"=="--help" goto usage

echo ERROR: Unknown option "%~1".
echo.
goto usage_error

:missing_value
echo ERROR: Option "%~1" requires a value.
echo.
goto usage_error

:arguments_parsed
call :validate_unsigned DOMAIN 0 232 || exit /b 2
call :validate_unsigned TARGETS 1 256 || exit /b 2
if defined CONTROL_DOMAIN (
    call :validate_unsigned CONTROL_DOMAIN 0 232 || exit /b 2
) else (
    if "%DOMAIN%"=="232" (
        set "CONTROL_DOMAIN=0"
    ) else (
        set /a CONTROL_DOMAIN=DOMAIN+1
    )
)
if "%CONTROL_DOMAIN%"=="%DOMAIN%" (
    echo ERROR: -ControlDomain must differ from -Domain.
    exit /b 2
)
if defined RUN_SECONDS (
    call :validate_unsigned RUN_SECONDS 1 604800 || exit /b 2
)

if /I not "%CONFIGURATION%"=="Debug" (
    if /I not "%CONFIGURATION%"=="RelWithDebInfo" (
        if /I not "%CONFIGURATION%"=="Release" (
            echo ERROR: -Configuration must be Debug, RelWithDebInfo, or Release.
            exit /b 2
        )
    )
)

if not defined CONNEXT_DIR if defined CONNEXTDDS_DIR set "CONNEXT_DIR=%CONNEXTDDS_DIR%"
if not defined CONNEXT_DIR if defined NDDSHOME set "CONNEXT_DIR=%NDDSHOME%"
if not defined CONNEXT_DIR set "CONNEXT_DIR=C:\Program Files\rti_connext_dds-7.7.0"

if not exist "%CONNEXT_DIR%\lib\x64Win64VS2017\" (
    echo ERROR: RTI Connext libraries were not found under:
    echo        "%CONNEXT_DIR%\lib\x64Win64VS2017"
    echo Pass the installation with -ConnextDir "C:\path\to\rti_connext_dds".
    exit /b 1
)

if not defined BUILD_DIR set "BUILD_DIR=%REPO_ROOT%\build\windows-x64"
for %%I in ("%BUILD_DIR%") do set "BUILD_DIR=%%~fI"
set "BIN_DIR=%BUILD_DIR%\%CONFIGURATION%"

if not exist "%BIN_DIR%\radar_app.exe" (
    if exist "%BUILD_DIR%\radar_app.exe" set "BIN_DIR=%BUILD_DIR%"
)
if not exist "%BIN_DIR%\radar_app.exe" (
    if exist "%REPO_ROOT%\bin\radar_app.exe" set "BIN_DIR=%REPO_ROOT%\bin"
)

call :require_executable radar_app.exe || exit /b 1
call :require_executable target_gen.exe || exit /b 1
call :require_executable target_control.exe || exit /b 1

set "QOS_FILE=%REPO_ROOT%\qos\radar_qos.xml"
if not exist "%QOS_FILE%" set "QOS_FILE=%BIN_DIR%\qos\radar_qos.xml"
if not exist "%QOS_FILE%" (
    echo ERROR: qos\radar_qos.xml was not found.
    exit /b 1
)

call :require_not_running radar_app.exe || exit /b 1
call :require_not_running target_gen.exe || exit /b 1
call :require_not_running target_control.exe || exit /b 1

set "PATH=%CONNEXT_DIR%\lib\x64Win64VS2017;%PATH%"
set "CONNEXTDDS_DIR=%CONNEXT_DIR%"
set "NDDSHOME=%CONNEXT_DIR%"
set "RADAR_QOS_FILE=%QOS_FILE%"

set "LOG_DIR=%BUILD_DIR%\demo-logs\cmd-%RANDOM%-%RANDOM%"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"
if errorlevel 1 (
    echo ERROR: Could not create log directory "%LOG_DIR%".
    exit /b 1
)

set "STOP_FILE=%LOG_DIR%\stop.signal"
if exist "%STOP_FILE%" del /q "%STOP_FILE%"

set "RADAR_ARGS=--domain %DOMAIN% --stop-file "%STOP_FILE%""
set "TARGET_ARGS=--domain %DOMAIN% --control-domain %CONTROL_DOMAIN% --targets %TARGETS% --stop-file "%STOP_FILE%""
set "CONTROL_ARGS=--domain %CONTROL_DOMAIN% --stop-file "%STOP_FILE%""
if defined RUN_SECONDS (
    set "RADAR_ARGS=%RADAR_ARGS% --run-seconds %RUN_SECONDS%"
    set "TARGET_ARGS=%TARGET_ARGS% --run-seconds %RUN_SECONDS%"
    set "CONTROL_ARGS=%CONTROL_ARGS% --run-seconds %RUN_SECONDS%"
)
if defined DISABLE_SUB_3KM set "RADAR_ARGS=%RADAR_ARGS% --disable-sub-3km"

echo Starting target generator on simulation domain %DOMAIN% and control domain %CONTROL_DOMAIN%...
start "Target Generator" /B /D "%REPO_ROOT%" "%BIN_DIR%\target_gen.exe" %TARGET_ARGS% 1>"%LOG_DIR%\target.stdout.log" 2>"%LOG_DIR%\target.stderr.log"
ping 127.0.0.1 -n 2 >nul

echo Starting target-control UI on control domain %CONTROL_DOMAIN%...
start "Target Control" /B /D "%REPO_ROOT%" "%BIN_DIR%\target_control.exe" %CONTROL_ARGS% 1>"%LOG_DIR%\control.stdout.log" 2>"%LOG_DIR%\control.stderr.log"

echo Starting radar UI on simulation domain %DOMAIN%...
echo Close the radar window to stop all three applications.
echo Logs: %LOG_DIR%
start "Radar UI" /B /WAIT /D "%REPO_ROOT%" "%BIN_DIR%\radar_app.exe" %RADAR_ARGS% 1>"%LOG_DIR%\radar.stdout.log" 2>"%LOG_DIR%\radar.stderr.log"

>"%STOP_FILE%" echo stop
echo Radar closed; stopping target_gen and target_control...
ping 127.0.0.1 -n 3 >nul
echo Demo stopped. Logs: %LOG_DIR%
exit /b 0

:validate_unsigned
set "VALUE_NAME=%~1"
call set "VALUE=%%%VALUE_NAME%%%"
if not defined VALUE (
    echo ERROR: -%VALUE_NAME% requires a value.
    exit /b 1
)
for /f "delims=0123456789" %%A in ("%VALUE%") do (
    echo ERROR: -%VALUE_NAME% must be an integer from %~2 to %~3.
    exit /b 1
)
if %VALUE% LSS %~2 (
    echo ERROR: -%VALUE_NAME% must be an integer from %~2 to %~3.
    exit /b 1
)
if %VALUE% GTR %~3 (
    echo ERROR: -%VALUE_NAME% must be an integer from %~2 to %~3.
    exit /b 1
)
exit /b 0

:require_executable
if exist "%BIN_DIR%\%~1" exit /b 0
echo ERROR: "%BIN_DIR%\%~1" was not found.
echo Build the RelWithDebInfo configuration before launching.
exit /b 1

:require_not_running
tasklist /FI "IMAGENAME eq %~1" 2>nul | find /I "%~1" >nul
if errorlevel 1 exit /b 0
echo ERROR: %~1 is already running. Close it before using start-all.cmd.
exit /b 1

:usage
echo Usage:
echo   start-all.cmd [-Domain N] [-ControlDomain N] [-Targets N]
echo                 [-Configuration Debug^|RelWithDebInfo^|Release]
echo                 [-ConnextDir PATH] [-BuildDir PATH] [-RunSeconds N]
echo                 [-DisableSub3km]
echo.
echo Defaults: simulation domain 92, control domain 93, 32 targets,
echo           RelWithDebInfo, and the standard Connext 7.7.0 location.
echo.
echo This is a native Command Prompt script and does not invoke PowerShell.
exit /b 0

:usage_error
call :usage
exit /b 2
