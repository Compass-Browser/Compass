@echo off
rem On Windows, python3 is usually the Microsoft Store stub: prefer the py launcher.
where py >nul 2>nul
if %ERRORLEVEL%==0 (
  py -3 "%~dp0compass" %*
) else (
  python "%~dp0compass" %*
)
exit /b %ERRORLEVEL%
