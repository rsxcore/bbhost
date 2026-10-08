@echo off
rem The second instance (invader): its own config, account and saves in inst3\, traces in http-trace-invader\.
rem BBHOST_WEX: "all" puts the Sinister Bell's sign in every area of wex-areas.txt at once,
rem "1" moves it round them one a minute, empty is the game as shipped.
rem BBHOST_INVADE_AREA: one fixed area instead, "AreaId,AreaRegionId" (wins over BBHOST_WEX).
set BBHOST_CONFIG_DIR=%~dp0inst3\config
set BBHOST_HTTP_TRACE=http-trace-invader
set BBHOST_NP_TRACE=1
set BBHOST_NP_TEST=probe
set BBHOST_WEX=all
set BBHOST_INVADE_AREA=
set BBHOST_INVADE_ON=both
call "%~dp0run-bbhost.bat" %*
