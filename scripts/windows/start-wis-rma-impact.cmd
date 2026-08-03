@echo off
call "%~dp0start-wis.cmd" -CfgName RmaOutageImpactLiveView %*
exit /b %ERRORLEVEL%
