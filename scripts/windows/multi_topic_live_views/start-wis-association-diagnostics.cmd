@echo off
call "%~dp0..\start-wis.cmd" -CfgName AssociationDiagnosticsLiveView %*
exit /b %ERRORLEVEL%
