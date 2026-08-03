@echo off
call "%~dp0start-wis.cmd" -CfgName TrackLossLiveView %*
exit /b %ERRORLEVEL%
