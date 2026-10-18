# Catálogos de bayo.sh

La pestaña **Descubrir** lee catálogos `.json`. El de bayo.sh es [`catalog.json`](catalog.json);
cualquiera puede publicar el suyo (en un repo de GitHub, GitHub Pages o cualquier servidor) y
agregarlo en la app con **Descubrir → Agregar catálogo**.

## Formato

```json
{
  "name": "Mis fondos",
  "items": [
    {
      "title": "Aurora Boreal",
      "category": "Paisajes",
      "thumb": "thumbs/aurora.jpg",
      "video": "https://ejemplo.com/aurora-boreal.mp4",
      "size_mb": 5,
      "fps": 30,
      "res": "1080p",
      "author": "Tu nombre",
      "license": "CC BY 4.0"
    }
  ]
}
```

- `title`, `video`: obligatorios. `video` debe ser el enlace **directo** al `.mp4`.
- `thumb`: imagen 16:9 (jpg/png/webp, idealmente 480×270 y menos de 100 KB).
- `thumb` y `video` pueden ser rutas relativas al `.json`.
- Categorías sugeridas (se traducen solas): Paisajes, Ciudad, Espacio, Retro, Abstracto,
  Naturaleza, Otros. Cualquier otra se muestra tal cual.
- Videos: H.264, 1920×1080 o menos, hasta 60 fps, con `+faststart`. Lo demás se marca como
  NO COMPATIBLE al bajarlo (se puede convertir desde el celular o la PC).

Publica solo videos que tengas derecho a compartir y pon el autor y la licencia de cada uno.

## Fondos del catálogo de bayo.sh

Hechos para el proyecto, en bucle perfecto de 30 s, 1080p30. Licencia **CC BY 4.0**
(úsalos y compártelos dando crédito a bayo.sh). Están en [`videos/`](videos).
