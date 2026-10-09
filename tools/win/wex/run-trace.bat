@echo off
rem Runs bbhost with the server-traffic traces on: bodies land in http-trace\, the log in logs\.
set BBHOST_HTTP_TRACE=1
set BBHOST_NP_TRACE=1
call "%~dp0run-bbhost.bat" %*
