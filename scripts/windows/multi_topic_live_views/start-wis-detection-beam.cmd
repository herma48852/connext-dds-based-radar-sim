@echo off
call "%~dp0..\start-wis.cmd" -CfgName DetectionBeamLiveView %*
exit /b %ERRORLEVEL%
