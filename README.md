<p align="center">
  <img src="assets/icon0.png" width="160" alt="bayo.sh">
</p>

<h1 align="center">bayo.sh</h1>

<p align="center"><b>Fondos de pantalla animados en video para el inicio de tu PS5.</b></p>

<p align="center"><b>🇲🇽 Español</b> · <a href="README.en.md">🇺🇸 English</a></p>

<p align="center">
  <img src="https://img.shields.io/badge/versión-0.4.0-22b8ff?style=flat-square" alt="versión">
  <img src="https://img.shields.io/badge/plataforma-PS5-22b8ff?style=flat-square" alt="plataforma">
  <img src="https://img.shields.io/badge/probado-13.60%20%2B%20etaHEN-22b8ff?style=flat-square" alt="probado">
  <img src="https://img.shields.io/badge/licencia-GPL--3.0-22b8ff?style=flat-square" alt="licencia">
</p>

---

## Qué hace

- **Tu video de fondo** en el inicio del PS5, en lugar de la imagen fija.
- **Fondo por juego**: cada juego puede tener su propio video al seleccionarlo.
- **Efectos**: oscurecer, desenfoque, degradados, viñeta, neblina y paralaje.
- **Sonido** en los videos que traen audio, con control de volumen.
- **Pausa al jugar**, para que el juego tenga todo el decodificador de video.
- **Fondo normal en Ajustes** y modo **solo en PlayStation Store**.
- **Aleatorio**: un video distinto en cada arranque.
- **Reposo**: regresa solo al despertar la consola.
- **App en el inicio** (Multimedia) para manejar todo con el control.
- **Desde el celular o la PC**: abre `http://IP-del-PS5:9050` para subir videos y controlar todo.
- **Luz del control**: el LED del DualSense sigue el color de tu fondo, o usa modos Fijo, Arcoíris, Respiración u Ola.
- **Cualquier video**: si no es compatible (4K, HEVC, 60+ fps…), el botón **Convertir y aplicar** lo deja en 1080p desde el celular o la PC.
- **Descargar desde un enlace**: pega el enlace de un .mp4 y el PS5 lo baja solo a tus fondos.
- **Descubrir**: catálogo de fondos animados con buscador y categorías; **Descargar → Usar** sin salir de la app.
- **WallpaperWaifu en Descubrir**: navega sus fondos (categorías, búsqueda, filtro 1080p) y dale a su botón verde de descarga; el PS5 lo guarda en tus fondos.
- **Widgets en el inicio**: tarjeta de cristal con clima de tu ciudad (o detectado solo), temperatura del PS5, espacio libre y batería del control. Los datos se mueven y cambian de color, y el contorno brilla en modo Ola, Neón, Arcoíris, Sigue al fondo, Fijo o Respiración.
- **QR para el celular**: escanéalo desde la guía o el inicio y se abre la app en tu teléfono.
- **Catálogos propios**: publica un `.json` con tus fondos y cualquiera lo agrega con su enlace ([formato](catalog/README.md)).
- **Rotación**: cambia de fondo cada 5, 15, 30 min o 1 h, aleatorio o en orden.
- **Tus sitios de fondos**: agrega las páginas o repos que quieras; se guardan en el PS5.
- **Música de fondo** en la página del celular/PC (fantasía oscura original, se apaga con un botón).
- **Guía de primer uso** dentro de la app.
- **10 idiomas**: español, English, Português, Français, Deutsch, Italiano, 日本語, 한국어, 中文, Русский.
- **Compatible con etaHEN** en firmware 13.60: no le cambia permisos al menú ni lee el kernel desde dentro, y si algo falla se detiene solo en vez de tumbar la consola.

## Instalación

Necesitas un PS5 con jailbreak y un cargador de payloads.

1. Copia tus videos `.mp4` a `/data/bayosh/video/` (por FTP), o a una carpeta `bayosh` en una USB.
2. Carga `bayosh.elf`.
3. Abre **bayo.sh** en Multimedia, elige tu video en **Fondos** y presiona **Usar**.

También puedes subir los videos desde el celular: abre `http://IP-del-PS5:9050` en el navegador (misma red Wi-Fi).

**Formato recomendado:** `.mp4` H.264, 1920×1080, hasta 60 fps. Para convertir cualquier video en Windows, arrástralo sobre `tools/convert_for_ps5.bat` (necesita `ffmpeg.exe`).

**Controles en la app:** L1 / R1 cambian de sección, ○ cierra ventanas.

### Antes de usarlo

- Pruébalo unos días lanzándolo a mano antes de meterlo a tu autoload.
- La primera vez crea una carpeta vacía en `/system_ex/vsh_asset/wallpaper`. No modifica archivos de Sony.
- Si algo sale mal, reinicia sin cargar el exploit y la consola arranca normal.
- El registro queda en `/data/bayosh/bayosh.log`. Mándalo si reportas un problema.

## Compilar

En Linux o WSL, con:

- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) (probado con v0.44) y clang 18
- [OnionHEN](https://github.com/aydencharles/onionHEN) en el tag `v0.9.0-beta2`

```bash
git clone https://github.com/aydencharles/onionHEN.git ~/onionHEN
git -C ~/onionHEN checkout v0.9.0-beta2
ONIONHEN=~/onionHEN PS5_PAYLOAD_SDK=~/ps5-payload-sdk tools/build.sh
```

El resultado queda en `dist/bayosh.elf`. El script aplica `patches/onionhen-detour.patch` (necesario para etaHEN), copia `src/` al árbol de OnionHEN y compila el objetivo `bayosh`.

### Estructura

| Archivo | Qué es |
|---|---|
| `src/bayosh_daemon.cpp` | El payload: lista videos, monta la carpeta, inyecta el módulo al menú y lo vigila |
| `src/bayosh_shellui.cpp` | El módulo que corre dentro del menú del PS5 y pone el video de fondo |
| `src/bayosh_web.cpp` | Servidor web en el puerto 9050 (la app y la página del celular) |
| `src/bayosh_tile.cpp` | El ícono de la app en el inicio |
| `src/bayosh_widgets.cpp` | Widgets del inicio (clima, temperatura, disco, batería) |
| `src/bayosh_tls.cpp` | HTTPS propio con mbedTLS (catálogos, descargas, clima) |
| `src/bayosh_page.html` | La interfaz (PS5, PC y celular) |

### API (puerto 9050)

`/api/state`, `/api/select?i=`, `/api/on`, `/api/off`, `/api/dim?v=`, `/api/volume?v=`, `/api/blur?v=`, `/api/look?k=&v=`, `/api/set?k=&v=`, `/api/games`, `/api/gamebg?tid=&i=`, `/api/delete?i=`, `POST /api/upload?name=`, `/api/reset`, `/api/shutdown`, `/api/catalog?url=`, `/api/img?url=`, `/api/download?url=&name=`, `/api/widgets?k=&v=`, `/video?i=`, `/thumb?i=`, `/gameicon?tid=`.

## Apoya el proyecto ☕

bayo.sh es gratis y siempre lo será. Si te gusta y quieres apoyar para que siga creciendo (más fondos, widgets, nuevas funciones), puedes invitarme un café:

<p align="center"><a href="https://paypal.me/fungibayo"><img src="https://img.shields.io/badge/Apoya%20a%20bayo.sh-PayPal-22b8ff?style=for-the-badge&logo=paypal&logoColor=white" alt="Apoya a bayo.sh con PayPal"></a></p>

Una ⭐ en el repo o compartirlo también ayuda muchísimo.

## Créditos

Hecho por **Josué** · [BayoTech](#).

bayo.sh es un proyecto derivado. El motor de video parte de **LiveShell** de ZiZc3 y usa el inyector, las librerías y el trabajo base de **[OnionHEN](https://github.com/aydencharles/onionHEN/)**. Los cambios de bayo.sh están en [CHANGELOG.md](CHANGELOG.md).

También usa: [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0), los certificados raíz de [certifi](https://github.com/certifi/python-certifi) (MPL-2.0), [qrcode-generator](https://github.com/kazuhikoarase/qrcode-generator) de Kazuhiko Arase (MIT), el clima de [Open-Meteo](https://open-meteo.com/) (CC BY 4.0), la búsqueda de lugares de [OpenStreetMap / Nominatim](https://nominatim.openstreetmap.org/) (© colaboradores de OpenStreetMap, ODbL) y la ubicación aproximada de [GeoJS](https://www.geojs.io/). Los fondos de WallpaperWaifu son de sus autores y se descargan desde su página.

## Licencia

GPL-3.0. Ver [LICENSE](LICENSE). Se entrega sin ninguna garantía; úsalo bajo tu propio riesgo.
