# Cambios

## v0.3.0 — 2026-10-17 (primera versión pública)
Reúne todas las betas de la v0.3:
- Fondos animados en video en el inicio del PS5 y fondo propio por juego (o el original del juego).
- Efectos: oscurecer, desenfoque, degradados, viñeta, neblina y paralaje.
- Luz del control: sigue al fondo, Fijo, Arcoíris, Respiración y Ola, con velocidades.
- App en el inicio con guía de primer uso y 10 idiomas.
- Control desde el celular o la PC; descargar videos desde un enlace directo al PS5.
- Sitios de fondos propios (cada quien agrega los suyos).
- Cerrar te regresa al inicio; con OnionHEN activo no reinicia el menú (evita congelar la consola).

Pendiente para v0.4: música dentro del PS5, luz que reacciona a los botones, efectos en los
cuadros del inicio, cerrar la ventana por completo sin reiniciar el menú.

## v0.3-beta11 — 2026-10-17
- Cerrar te saca al inicio en el momento (antes esperaba a que el motor terminara).
- Conseguir fondos: sin sitios de fábrica; cada quien agrega sus páginas o repos. Se guardan en el
  PS5 (/data/bayosh/sites.txt), así se ven igual desde el PS5 y desde el celular.

## v0.3-beta10 — 2026-10-17
- /system_tmp lleno (errno 28): los archivos de control se escriben encima sin truncar antes, así
  no necesitan espacio nuevo. En la beta9 el "off" del video no se pudo escribir y el fondo seguía
  al cerrar.
- Cerrar: diagnóstico. La ventana es una web app del propio menú (sin proceso ni app id), así que
  solo el menú puede cerrarla. El módulo lista las clases/métodos del menú que sirven para cerrar
  apps (`closeapi` en el registro) para usar el correcto en la siguiente versión.

## v0.3-beta9 — 2026-10-17
- Fondos → Conseguir fondos ahora funciona desde el PS5: **Abrir** lanza el navegador del PS5 en
  ese sitio, y **Descargar desde un enlace** baja el .mp4 directo a /data/bayosh/video por HTTPS
  (libSceHttp2 del sistema, cargada al momento). Barra de progreso y botón Cancelar. Comprueba que
  lo bajado sea un .mp4 de verdad. También sirve desde el celular: pegas el enlace y descarga el PS5.
- Cerrar: con otro mod del menú activo (OnionHEN) ya no se ofrece reiniciar el menú (en la beta8
  congeló la consola); el motor también lo rechaza. Al cerrar te lleva al inicio solo
  (sceSystemServiceNavigateToGoHome).
- Música: el registro de la beta8 mostró el audio decodificándose (44977 bytes) pero sin salida:
  la ventana del menú del PS5 no tiene sonido. En el PS5 se oculta; en celular/PC sigue.

## v0.3-beta8 — 2026-10-17
- Cerrar ahora pregunta cómo: **Cerrar rápido** (apaga fondo y luz; la ventana se queda, presiona PS)
  o **Cerrar y reiniciar el menú** (se cierra todo, también la ventana; con OnionHEN u otros mods
  del menú hay que volver a lanzarlos). El registro dice qué detectó (`stop: onion …`).
- Música: arranca a volumen completo (sin subida gradual, que en el PS5 se quedaba en silencio) y
  a los 3 s escribe en el registro su estado real (`music: vol=… muted=… paused=…`).
- Fondos → Conseguir fondos: Pexels, Pixabay, Mixkit y Coverr (licencia libre) y **Agregar sitio**
  para que cada quien sume sus propias páginas (se guardan solo en ese navegador).
- La guía traduce los botones Siguiente / ¡Empezar!.
- Icono nuevo (assets/icon0.png), en el PS5 y en GitHub. Al iniciar también se actualiza el icono
  que el menú guarda en caché (/user/appmeta/BYSH00001).

## v0.3-beta7 — 2026-10-17
- Idiomas: español, English, Português, Français, Deutsch, Italiano, 日本語, 한국어, 中文 y Русский.
  Botón 🌐 arriba y Ajustes → Idioma. Las traducciones están en tools/i18n.py
  (python3 tools/apply_i18n.py las pega en la página).
- Música: va dentro de un video diminuto (/music.mp4), porque el navegador del PS5 rechaza
  archivos de solo audio (NotSupportedError).
- Cerrar apaga la luz del control antes de salir.
- Pie de página: © 2026 Josué.

## v0.3-beta6 — 2026-10-17
- Si ya hay una bayo.sh dentro del menú (por ejemplo, porque el menú no se reinició al cerrar
  la anterior), no se mete una segunda encima: avisa que reinicies la consola. Dos módulos en
  el mismo menú lo congelaban (prueba v0.3-beta5).
- Cerrar comprueba que el menú de verdad se reinició; si no, conserva la marca y avisa.

## v0.3-beta5 — 2026-10-17
- Cerrar: la ventana de la app no es una app "normal" (no la encuentra por su Title ID), así que
  el motor busca el proceso que la corre (el que lleva el Title ID BYSH00001) y lo cierra.
  Si no la encuentra, deja en el registro la lista de apps abiertas para seguir buscando.

## v0.3-beta4 — 2026-10-17
- Música: el navegador del PS5 no suena con síntesis en vivo; ahora la pieza va dentro del
  payload como archivo (src/bayosh_music.mp4, 58 s en bucle; el audio va dentro de un video diminuto porque el PS5 no reproduce solo audio).
  La fuente para regenerarla está en tools/music_source.js.
- Respiración usa el color que eliges en la app (antes tomaba el del video).

## v0.3-beta — 2026-10-17
- Luz del control con modos: Sigue al fondo, Fijo, Arcoíris, Respiración y Ola, con tres
  velocidades y paleta de colores. La animación corre dentro del menú (~20 cambios por segundo).
- Música de fondo en la app: pieza de fantasía oscura (cuerdas, coro, arpa, campanas), original y generada en vivo (sin archivos de audio),
  con botón en la barra de arriba e interruptor en Ajustes.
- "Cerrar" también cierra la ventana de la app (y avisa si el sistema la deja abierta).

## v0.2-beta10 — 2026-10-17
- Encadenamiento confirmado en consola ("chained -> ok", sin congelarse).
- Luz del control: el menú no tiene sesión propia del control (GetHandle -> 0x80920008), así
  que abre una solo para la luz (scePadOpen). Se registra el resultado.

## v0.2-beta9 — 2026-10-17
- Causa del congelamiento intermitente encontrada: otro mod del menú (etaHEN u otro payload)
  a veces ya había enganchado Application.Update con un salto absoluto (ff 25 + dirección).
  El detour copiaba ese salto a nuestro trampolín, que entonces leía NUESTRA dirección: el
  menú se llamaba a sí mismo sin fin y se congelaba. Ahora bayo.sh lo detecta y se encadena:
  cambia solo la dirección del salto (atómico) y llama al gancho del otro mod como original.

## v0.2-beta8 — 2026-10-17 (diagnóstico)
- El enganche al menú escribe cada paso en shellui.log (dirección del trampolín, bytes del
  objetivo, tamaños) y prueba primero que la memoria del trampolín se pueda ejecutar.
  Sirve para encontrar el congelamiento intermitente al arrancar.

## v0.2-beta7 — 2026-10-17
- El enganche al menú (Application.Update) se reintenta hasta 6 veces si falla, en vez de
  rendirse a la primera. Registra alineación y permisos de la página para diagnosticar.

## v0.2-beta6 — 2026-10-17
- Luz del control: acepta cualquier número de control válido (antes se descartaban los
  grandes) y prueba los tres tipos de puerto; registra cada intento en shellui.log.

## v0.2-beta5 — 2026-10-17
- PS5: el fondo animado vuelve, pero dibujado a media resolución, con menos elementos, y se
  pausa solo mientras te mueves con el control para que los menús respondan al instante.

## v0.2-beta4 — 2026-10-17
- Luz del control: el color ahora se manda desde el ciclo que vigila el inicio (el hilo
  aparte no llegaba a correr). Más registro para ver cada paso.

## v0.2-beta3 — 2026-10-17
- Juegos: nueva opción "Original del juego" (su fondo y su música del PS5, sin video), por
  juego o para todos de una vez. Los juegos con video propio no cambian.
- Modo ligero en el PS5: sin capas en movimiento ni desenfoques en vivo, para que la app vaya
  fluida con el control. Se puede activar el fondo animado en Ajustes.

## v0.2-beta — 2026-10-17
- Interfaz con más vida: aurora de colores animada y lluvia de código a color que se tiñe
  con el color del video que tienes puesto.
- Animaciones tipo iOS: indicador de sección deslizante, cambio de sección con empuje,
  interruptores y selectores con rebote, ventanas tipo hoja (en celular suben desde abajo y
  se cierran deslizando), avisos tipo Dynamic Island, contadores animados.
- Guía de 5 pasos al abrir la app por primera vez (se guarda en la consola, se vuelve a
  abrir con el botón ? o en Ajustes).
- Luz del control (experimental): el LED del DualSense toma el color del fondo. El color de
  cada video se mide solo y se puede cambiar a mano. Viene apagada por defecto.
- Avisos del sistema en español.

## v0.1 — 2026-10-17
- Interfaz nueva de bayo.sh: una sola app adaptable para PS5 (con control), PC y celular.
  Secciones Inicio, Fondos, Juegos, Efectos y Ajustes; subida de videos con progreso,
  miniaturas automáticas, selector de fondo por juego, restablecer con confirmación.
- La página del celular ahora abre la misma interfaz.
- Todo renombrado a bayo.sh: payload `bayosh.elf`, carpetas `/data/bayosh` y `/system_tmp/bayosh`,
  registro `bayosh.log`, carpeta `bayosh` en USB, ícono y textos propios.

## v0.0.10 — 2026-10-17
Compatibilidad con etaHEN en firmware 13.60 (antes: congelamiento del menú y kernel panic):
- Nunca se cambia el authid de SceShellUI.
- Dentro del menú no hay lecturas ni escrituras de kernel.
- El payload resuelve desde fuera los símbolos de Mono y `sceKernelMprotect` y se los pasa al
  módulo por UDP local (127.0.0.1:9052).
- Sin respaldo `kernel_mprotect` en los detours (`patches/onionhen-detour.patch`).
- Si falta algo, el módulo se detiene solo y avisa en vez de forzar.

## v0.0.1 — 2026-10-16
- Primera versión: derivado de LiveShell (GPL-3.0) con rutas, ícono y nombre de bayo.sh.
