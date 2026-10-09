@echo off
rem The first instance (host): your usual config and account. Traces in http-trace\.
rem The probe logs "stateInfo191=1" while the maiden's effect (invadable) is on the host;
rem the watched flags are Central Yharnam's maiden.
set BBHOST_HTTP_TRACE=http-trace
set BBHOST_NP_TRACE=1
set BBHOST_NP_TEST=probe
set BBHOST_NP_WATCH_FLAGS=12414220,12414221,12414222,12414223
call "%~dp0run-bbhost.bat" %*
