@echo off
call "%~dp0start-wis.cmd" -CfgName AssociationDiagnosticsLiveView %*
exit /b %ERRORLEVEL%
