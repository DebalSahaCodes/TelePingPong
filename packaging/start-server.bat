@echo off
title Ping Pong server
echo Starting the Ping Pong server on TCP port 7777.
echo Players connect to this computer's PUBLIC address (see README.txt - firewall / port forwarding).
echo Close this window or press Ctrl+C to stop the server.
echo.
pingpong.exe server --port 7777
pause
