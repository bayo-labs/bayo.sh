@echo off
rem Convertidor de videos de bayo.sh. Arrastra uno o varios videos sobre este archivo.
rem Cualquier formato (mkv, mov, avi, webm, mp4, 4K, 120 fps...) -> <nombre>_ps5.mp4 junto al original:
rem H.264 1920x1080 (los mas chicos llevan barras negras), max 60 fps, audio AAC.
rem Necesita ffmpeg.exe en esta carpeta o en el PATH.
setlocal
set FFMPEG=%~dp0ffmpeg.exe
if not exist "%FFMPEG%" set FFMPEG=ffmpeg
"%FFMPEG%" -version >nul 2>&1
if errorlevel 1 (
  echo No encontre ffmpeg.exe. Ponlo en la misma carpeta que este archivo.
  pause
  exit /b 1
)
if "%~1"=="" (
  echo Arrastra uno o varios videos sobre convert_for_ps5.bat.
  pause
  exit /b 1
)

:next
if "%~1"=="" goto done
echo.
echo === %~nx1
"%FFMPEG%" -hide_banner -loglevel error -stats -y -i "%~1" ^
  -vf "scale=1920:1080:force_original_aspect_ratio=decrease:flags=lanczos,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,format=yuv420p" ^
  -fpsmax 60 -c:v libx264 -preset slow -profile:v high -level 4.2 -b:v 16M -maxrate 20M -bufsize 20M ^
  -x264-params "keyint=60:min-keyint=30:scenecut=0:open-gop=0" ^
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv ^
  -c:a aac -b:a 192k -ar 48000 -ac 2 ^
  -movflags +faststart "%~dpn1_ps5.mp4"
if errorlevel 1 (echo FALLO: %~nx1) else (echo LISTO: %~n1_ps5.mp4)
shift
goto next

:done
echo.
echo Listo. Copia los _ps5.mp4 a /data/bayosh/video en el PS5, a una carpeta "bayosh\video"
echo en una USB, o subelos desde bayo.sh en tu celular o PC (IP-del-PS5:9050).
pause
