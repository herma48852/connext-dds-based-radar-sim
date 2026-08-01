@echo off
setlocal EnableExtensions DisableDelayedExpansion

for %%I in ("%~dp0..\..") do set "REPO_ROOT=%%~fI"

set "CONNEXT_DIR="
if defined CONNEXTDDS_DIR set "CONNEXT_DIR=%CONNEXTDDS_DIR%"
if not defined CONNEXT_DIR if defined NDDSHOME set "CONNEXT_DIR=%NDDSHOME%"
if not defined CONNEXT_DIR set "CONNEXT_DIR=C:\Program Files\rti_connext_dds-7.7.0"

set "CONFIG_FILE=%REPO_ROOT%\config\radar_live_view_wis.xml"
set "CONFIG_NAME=RadarLiveViews"
set "DOCUMENT_ROOT=%REPO_ROOT%\docs"
set "LISTENING_PORTS=18080"
set "VERBOSITY=3"
set "ENABLE_BUILTIN_TOPICS="

:parse
if "%~1"=="" goto launch
if /I "%~1"=="-Help" goto usage
if /I "%~1"=="--help" goto usage
if /I "%~1"=="-ConnextDir" (
    if "%~2"=="" goto missing_value
    set "CONNEXT_DIR=%~2"
    shift
    shift
    goto parse
)
if /I "%~1"=="-ConfigFile" (
    if "%~2"=="" goto missing_value
    set "CONFIG_FILE=%~2"
    shift
    shift
    goto parse
)
if /I "%~1"=="-CfgName" (
    if "%~2"=="" goto missing_value
    set "CONFIG_NAME=%~2"
    shift
    shift
    goto parse
)
if /I "%~1"=="-DocumentRoot" (
    if "%~2"=="" goto missing_value
    set "DOCUMENT_ROOT=%~2"
    shift
    shift
    goto parse
)
if /I "%~1"=="-ListeningPorts" (
    if "%~2"=="" goto missing_value
    set "LISTENING_PORTS=%~2"
    shift
    shift
    goto parse
)
if /I "%~1"=="-Verbosity" (
    if "%~2"=="" goto missing_value
    set "VERBOSITY=%~2"
    shift
    shift
    goto parse
)
if /I "%~1"=="-EnableBuiltinTopics" (
    set "ENABLE_BUILTIN_TOPICS=-enableBuiltinTopics"
    shift
    goto parse
)

echo ERROR: Unknown option: %~1
echo.
goto usage_error

:launch
set "WIS_LAUNCHER=%CONNEXT_DIR%\bin\rtiwebintegrationservice.bat"
if not exist "%WIS_LAUNCHER%" (
    echo ERROR: RTI Web Integration Service was not found at:
    echo   %WIS_LAUNCHER%
    echo Set CONNEXTDDS_DIR or NDDSHOME, or pass -ConnextDir PATH.
    exit /b 1
)
if not exist "%CONFIG_FILE%" (
    echo ERROR: WIS configuration was not found at:
    echo   %CONFIG_FILE%
    exit /b 1
)
if not exist "%DOCUMENT_ROOT%" (
    echo ERROR: Document root was not found at:
    echo   %DOCUMENT_ROOT%
    exit /b 1
)

echo Starting RTI Web Integration Service
echo   config:        %CONFIG_FILE%
echo   configuration: %CONFIG_NAME%
echo   document root: %DOCUMENT_ROOT%
echo   listening:     %LISTENING_PORTS%
echo.

if defined ENABLE_BUILTIN_TOPICS (
    call "%WIS_LAUNCHER%" -cfgFile "%CONFIG_FILE%" -cfgName "%CONFIG_NAME%" -enableWebSockets -documentRoot "%DOCUMENT_ROOT%" -listeningPorts "%LISTENING_PORTS%" -verbosity "%VERBOSITY%" -enableBuiltinTopics
) else (
    call "%WIS_LAUNCHER%" -cfgFile "%CONFIG_FILE%" -cfgName "%CONFIG_NAME%" -enableWebSockets -documentRoot "%DOCUMENT_ROOT%" -listeningPorts "%LISTENING_PORTS%" -verbosity "%VERBOSITY%"
)
exit /b %ERRORLEVEL%

:missing_value
echo ERROR: %~1 requires a value.
echo.

:usage_error
call :usage_text
exit /b 2

:usage
call :usage_text
exit /b 0

:usage_text
echo Usage: scripts\windows\start-wis.cmd [options]
echo.
echo Launch the Section 4 multi-topic live views through RTI Web Integration
echo Service. Defaults: RadarLiveViews, docs document root, port 18080.
echo.
echo Options:
echo   -ConnextDir PATH        RTI Connext installation
echo   -ConfigFile PATH       WIS XML configuration
echo   -CfgName NAME          web_integration_service name
echo   -DocumentRoot PATH     Static web document root
echo   -ListeningPorts PORTS  WIS port list, for example 18080 or 18080,18443s
echo   -Verbosity 0..6        WIS logging verbosity
echo   -EnableBuiltinTopics   Enable DDS built-in topics
echo   -Help                  Show this help
goto :eof
