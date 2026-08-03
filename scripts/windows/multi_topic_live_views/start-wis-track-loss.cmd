@echo off
call "%~dp0..\start-wis.cmd" -CfgName TrackLossLiveView %*
exit /b %ERRORLEVEL%
