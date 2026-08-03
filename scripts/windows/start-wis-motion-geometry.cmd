@echo off
call "%~dp0start-wis.cmd" -CfgName MotionGeometryLiveView %*
exit /b %ERRORLEVEL%
