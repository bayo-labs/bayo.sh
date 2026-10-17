@echo off
rem Cierra bayo.sh en el PS5 (igual que "Cerrar bayo.sh" en Ajustes de la app).
rem Uso: shutdown_bayosh.bat [IP del PS5]
setlocal
set HOST=%~1
if "%HOST%"=="" set /p HOST=IP del PS5 (sale arriba a la derecha en bayo.sh):
if "%HOST%"=="" exit /b 1
echo Cerrando bayo.sh en %HOST%...
powershell -NoProfile -Command "$c = New-Object Net.Sockets.TcpClient; try { $c.Connect('%HOST%', 9049); $r = New-Object IO.StreamReader($c.GetStream()); Write-Host ('bayo.sh: ' + $r.ReadLine()); $c.Close() } catch { Write-Host 'bayo.sh no esta corriendo en esa IP (puerto 9049 cerrado).' }"
echo Listo.
pause
