@echo off
cd /d D:\github\openppp2
git add -A
git commit -m "fix: add missing P2PDefs.h and VirtualEthernetTcpMss.h"
git push origin master
