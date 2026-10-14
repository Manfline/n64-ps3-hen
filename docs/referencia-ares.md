# Comparar un cuadro con ares

ares dibuja con el RDP a bajo nivel: es la referencia exacta de lo que
dibuja la consola. Sus estados guardados no sirven aquí, pero su volcado de
la RDRAM sí: lleva la display list del cuadro, sus datos y el búfer de
imagen que el RDP dibujó con ellos. `salsa64-run --ares` dibuja esa misma
lista con nuestra HLE gráfica y compara píxel a píxel, sin depender de
llegar a la misma escena en los dos emuladores.

## Volcado

1. Llevar el juego en ares a la escena y pausarlo (así la RDRAM y la DMEM
   son del mismo momento).
2. Tools → Memory, con RDRAM elegida, Export. ares lo guarda sin diálogo
   junto a la ROM, como `<ROM>-rdram-<fecha>.bin`: 4 u 8 MB (con el
   Expansion Pak), big-endian como la RDRAM. Opcional: también RSP DMEM (la herramienta
   busca `<ROM>-rsp-dmem-<fecha>.bin` al lado, o se le da con `--dmem`).
3. Moverlo a `build/ares/` (git lo ignora). Los volcados y las imágenes que
   salen de ellos llevan datos del juego: **nunca** van al repositorio.

## Uso

```sh
tools/ares-comparar.sh build/ares/<juego>-rdram-<fecha>.bin
```

El script compila `salsa64-run` nativo (sin sanitizadores) y corre
`salsa64-run --ares` con las opciones que se le pasen (`--help`): `--task`
(una lista), `--fb` (un búfer), `--tol`, `--q5`, `--out DIR`, `--no-png`,
`--gfxdump`, `--pasadas N`.
Acepta varios volcados.

Por cada volcado:

- Busca las OSTask gráficas que quedaron en la RDRAM (el planificador de
  libultra guarda las últimas; con el volcado de la DMEM, también la de
  0xFC0) y reconoce el microcódigo como la HLE (`salsa64_rsp_detect_gbi`).
- Corre la HLE dos veces: la primera para saber en qué color images dibuja
  la lista; la segunda sobre una copia de la RDRAM con esos búferes en
  negro, que el rasterizador del host (`src/host/raster.c`) dibuja a 1×.
  Sin el negro, el rasterizador cebaría el búfer con la imagen de la
  consola y lo que la HLE no dibuja saldría igual.
- Compara cada color image de 16 bits que dibuja la lista (menos la z
  image) con la del volcado: PSNR por píxel y en promedios de 4×4, la
  diferencia media por canal, los píxeles que difieren más de `--tol` pasos
  de 5 bits (en 4×4), su caja y un mapa de 8×6, y tres PNG:
  `<volcado>-<lista>-<búfer>-{hle,consola,dif}.png` (en `dif`, la consola
  en gris oscuro y lo distinto de amarillo, poco, a rojo, mucho).

## Cómo leerlo

- **Doble búfer.** Una de las listas suele ser la del cuadro que el CPU
  está armando: «dibuja en la color image 000000» (segmento aún sin poner)
  o sin G_RDPFULLSYNC. Esa no se compara; la otra sí, contra el búfer que
  ella misma dibuja.
- **Tramado.** El RDP suma un tramado (magic square, Bayer o ruido) antes
  de quedarse con 5 bits por canal: por píxel es ruido de un paso. El
  promedio de 4×4 lo quita. La HLE se dibuja a 8 bits, como en el RSX, y
  la consola se toma como 8·v (5 bits sin expandir), que con tramado es
  insesgado; si el juego no trama, la consola sale ~3,5/255 más oscura.
  `--q5` dibuja la HLE a 5 bits truncados, como la copia a la RDRAM.
- **Lo que la lista no cubre** sale negro en la HLE; en la consola queda
  el cuadro de hace dos campos.
- **Render a textura.** Con una pasada, lo que la HLE lee de una imagen
  auxiliar por la RDRAM es lo que dejó la consola, y eso tapa los fallos
  de ese camino. `--pasadas 2` (o más) borra antes todas las color images
  que dibuja la lista y la corre dos veces seguidas, como dos cuadros: lo
  que se lee por la RDRAM es lo que el rasterizador copió ahí, como en la
  PS3 (así salió la sombra de Conker, docs/estado.md).
- **Límites del rasterizador del host:** muestrea en el centro del píxel,
  filtra bilineal (la N64, de tres puntos), no tiene la cobertura del AA
  (la consola escribe los píxeles cubiertos en parte: objetos ~½ px más
  gruesos) y mezcla a 8 bits (el blender de la N64 usa 5 bits de alfa).
  Eso da diferencias de un píxel en los bordes y amarillo en las texturas;
  lo que importa son las zonas rojas o con forma.
- **Qué dibujo es.** Con un build `EXTRA_CFLAGS=-DSALSA64_GFX_TRACE`,
  `SALSA64_GFX_VERBOSE=1 SALSA64_GFX_PIX=x,y` lista los dibujos que
  escriben ese píxel y el estado con que se armó cada uno, como con
  `--trace-gfx`.
- **Alta resolución:** el rasterizador es de 320×240; para búferes de 640
  de ancho, compilar con `-DRASTER_W=640 -DRASTER_H=480`.
- **Grietas y aristas:** el rasterizador hace, como la PS3, la pasada de
  aristas de los dibujos opacos con AA (docs/rsx-notes.md);
  `SALSA64_AA_EDGES=0` la quita, y `SALSA64_AA_EDGE_WIDTH=n` (1,5 px) y
  `SALSA64_AA_EDGE_OFFSET=factor,unidades` (8,64) cambian su ancho y su
  desplazamiento hacia atrás, para medir contra la consola.
- **En el RSX:** `--gfxdump` guarda la salida de la HLE de cada lista como
  el volcado de la PS3 (`<volcado>-<lista>.gfxdump.bin`, big-endian):
  `tools/gfxdump.py` lista sus dibujos (`pixel X Y`: cuáles cubren un
  píxel) y, copiado a `dev_hdd0/salsa64`, `embedded` = `dump:<nombre>`
  lo dibuja en el RSX (RPCS3 o la consola) y deja `replay.ppm`.

Lo medido con cada juego va en docs/estado.md.
