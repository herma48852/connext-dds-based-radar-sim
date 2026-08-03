@echo off
call "%~dp0..\start-wis.cmd" -CfgName RmaOutageImpactLiveView %*
exit /b %ERRORLEVEL%
