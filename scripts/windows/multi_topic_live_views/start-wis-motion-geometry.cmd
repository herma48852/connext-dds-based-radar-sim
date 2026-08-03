@echo off
call "%~dp0..\start-wis.cmd" -CfgName MotionGeometryLiveView %*
exit /b %ERRORLEVEL%
