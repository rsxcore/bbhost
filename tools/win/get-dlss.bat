@echo off
rem Downloads NVIDIA DLSS (nvngx_dlss.dll) beside bbhost.exe: see get-dlss.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0get-dlss.ps1"
pause
