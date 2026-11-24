@echo off
rem MESHtop: make the Chicago map pack (90 miles each way, zoom 6-14). Extra options can be added after the name.
cd /d "%~dp0"
where node >nul 2>nul
if errorlevel 1 (
  echo Node.js is not installed. Get the LTS version from https://nodejs.org, then run this again.
  pause
  exit /b 1
)
if not exist "node_modules\@maplibre\maplibre-gl-native" (
  echo Installing what the map maker needs, once...
  call npm install --no-audit --no-fund
  if errorlevel 1 (
    pause
    exit /b 1
  )
)
node make-map-pack.mjs %*
echo.
pause
