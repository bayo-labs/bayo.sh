# Cambios

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
