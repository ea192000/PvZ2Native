# PvZ2Native

📖 **Español** · [English](README.md)

**Ejecuta la librería nativa real de *Plants vs. Zombies 2* para Android (`libPVZ2.so`, ARM32) en PC.** Lo único que se emula es el **procesador**: PvZ2 solo existe compilado para ARM (no hay build x86/x64), así que un JIT traduce sus instrucciones ARM a x86_64. Todo lo demás —Android, JNI, libc, OpenGL ES, sistema de archivos— **no se emula, se reimplementa de forma nativa**, igual que hace **Wine** con Windows.

Este repositorio es una rama personalizada de [OptiJuegos/PvZ2Native](https://github.com/OptiJuegos/PvZ2Native) con mejoras orientadas a PC (ajustes en el juego, atajos de teclado, mando, confirmación al salir, etc.). Código original: https://github.com/OptiJuegos/PvZ2Native

> [!WARNING]
> Proyecto experimental. **No incluye el juego**: debes aportar tu propio `libPVZ2.so` y tu propio `.obb`, obtenidos legalmente desde la versión de Android.

---

## ✨ Qué incluye este build

Enfocado en *Plants vs. Zombies 2: It's About Time* **v4.5.2** en Windows y Linux, con una experiencia más cómoda con ratón, teclado o mando.

### Integración visual y de escritorio
- Icono propio del ejecutable y cursor personalizado (escala con el tamaño de ventana/pantalla).
- Pantalla de carga al arrancar y barra de progreso (evita una pantalla negra larga).
- En Windows, metadatos de versión del ejecutable mejorados.

### Ajustes dentro del juego
- Se abren con `Esc` o desde el menú principal con el botón de **Google Play** (esquina inferior derecha).
- Idioma, moneda, pantalla completa, monitor/pantalla, límite de FPS, compras gratuitas, eventos, atajos de teclado y opciones de mando, sin editar archivos a mano.
- La mayoría de cambios se aplican al momento (incluido mover la ventana a otro monitor). Cambiar el idioma, las compras gratuitas o las opciones de Eventos relanza el proceso para que el motor las recargue.
- Un `config.ini` viejo o incompleto se corrige al arrancar: faltantes con valores seguros; valores inválidos se sanean.

### Eventos
- Ajustes → **Eventos**: **Activar eventos** (`[runtime] use_cdn`) enciende o apaga el CDN emulado que alimenta los eventos y el contenido diario; **Forzar eventos locales** (`[runtime] rewrite_cdn`) vuelve a copiar los archivos de eventos del OBB en cada arranque. Cualquiera de los dos cambios relanza el juego.

### Logros
- Logros locales que se desbloquean al jugar; el progreso se guarda en `prefs.dat`.
- Toast animado al desbloquear (con SFX); lista completa desde ajustes (**Ver logros**).
- Opcional: silenciar toast/SFX desde ajustes o con `[game] achievement_notifications = 0` — los desbloqueos se siguen guardando.

### Confirmación al salir
- Confirmar o cancelar antes de cerrar (ratón, teclado o mando).
- `Esc` abre la pantalla de ajustes; un segundo `Esc` desde ajustes abre este diálogo (fijo, no remapeable). `F11` siempre alterna pantalla completa.

### Configuración
- Detección automática en el primer arranque (idioma, moneda, etc.).
- Consola/log oculta por defecto (`[log] console = 0`).
- Lo cotidiano va en la pantalla de ajustes; las claves avanzadas siguen en `config.ini`.
- Parámetros por línea de comandos para casi todas las opciones, p. ej. `--fullscreen=0`, `--video.fullscreen=0`, `--console=1`.

### Teclado y ratón
- Atajos configurables que simulan toques en pantalla (UI pensada para táctil).
- Por defecto: `1`–`8` plantas, `Space` velocidad, `Enter` pausa, `F` Plant Food, `S` pala, `Q`/`W`/`E` poderes, `R` replay, `Backspace` atrás, más almanaque / connect / vasebreaker / zen.
- Se editan desde la pantalla de ajustes.
- Rueda del ratón para listas (gesto de arrastre), clic derecho = Atrás, clic medio = Menú donde aplica.
- Correcciones de clics en pantalla completa / redimensionado y recuperación defensiva de entrada.

### Mando
- Stick izquierdo mueve el cursor; **A** confirma/clic.
- D-pad salta entre controles y celdas del césped; hombros cambian de zona de menú.
- Stick derecho arrastra (tipo swipe); acciones remapeables (pausa, poderes, Plant Food, pala, …).
- Sliders de velocidad y zona muerta; elegir qué mando usar o desactivarlos.
- Ratón, teclado y mando funcionan a la vez.

### Otros
- Campos de texto más fiables (clic fuera para salir del modo escritura).
- Mejor ritmo del limitador de FPS y menos ruido de log cuando está desactivado.
- Enlaces de Ayuda / Licencia / Privacidad en el navegador del sistema.
- ID de jugador persistente por instalación.

---

## 📖 Qué es (y qué NO es)

No es un *port* del código fuente del juego. Es un **híbrido de dos técnicas**, y la distinción importa:

- **Solo la CPU se emula.** El binario del juego es código máquina ARM y PvZ2 nunca se compiló para x86/x64, así que no hay forma de ejecutarlo directamente en un procesador de PC. Un JIT (dynarmic) lee esas instrucciones ARM y las traduce a x86_64 sobre la marcha. Esta es la única parte "emulada".
- **Android NO se emula: se reimplementa.** Cuando el juego llama a una función de Android (abrir un archivo, dibujar con OpenGL ES, reservar memoria, invocar Java…), esa llamada la atiende **código nativo de PC** que hace el trabajo real usando la API equivalente del escritorio. No hay un Android virtual corriendo debajo; hay una **reimplementación** de las funciones que el juego necesita.

Es exactamente el enfoque de **Wine** — *"Wine Is Not an Emulator"* — llevado un paso más allá: Wine reimplementa las llamadas de Windows sin emular nada porque los `.exe` ya son x86; aquí, como el juego es ARM, además hay que emular la CPU. El resto de la filosofía es idéntica: **traducir llamadas, no simular una máquina completa.**

```text
┌──────────────────────────────────────────────┐
│  libPVZ2.so  (ELF ARM32, EABI5, sin tocar)   │   ← el juego real de Android
└───────────────────────┬──────────────────────┘
                        │  instrucciones ARM
                        ▼
┌──────────────────────────────────────────────┐
│  dynarmic  —  JIT ARM32 → x86_64             │   ← ÚNICO componente emulado
└───────────────────────┬──────────────────────┘     (solo la CPU)
                        │  llamadas a "Android"
                        ▼
┌──────────────────────────────────────────────┐
│  Capa de compatibilidad (este proyecto)      │   ← reimplementación NATIVA,
│                                              │     estilo Wine (nada emulado)
│  JNI / JavaVM falso   ·   libc / libm / libz │
│  OpenGL ES → OpenGL   ·   OpenSL ES → SDL    │
│  GuestHeap propio     ·   VFS (rutas + .obb) │
└───────────────────────┬──────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────┐
│  Windows / Linux x64  ·  SDL2  ·  OpenGL 2.0 │
└──────────────────────────────────────────────┘
```

Cuando el juego pide algo del sistema operativo (abrir un archivo, reservar memoria, dibujar, reproducir sonido, llamar a Java), esa llamada sale del código emulado como una interrupción y la atiende un *handler* de esta capa, respaldado por la API equivalente de PC.

---

## 🧩 Arquitectura

El código está en [pvz2native/](pvz2native/) y se divide en capas con una regla estricta: **solo una capa puede contener direcciones concretas del `.so`.** Eso es lo que hace que soportar otra versión del juego sea sumar datos, no reescribir lógica.

| Capa | Pregunta que responde | ¿Puede tener una dirección del `.so`? |
| --- | --- | :---: |
| [runtime/](pvz2native/src/runtime/) | Cómo ejecutar código ARM (JIT, heap, hilos, sincronización) | **Nunca** |
| [game/symbols.cpp](pvz2native/src/game/symbols.cpp) | Dónde está cada cosa en *este* binario | **La única que sí** |
| [engine/](pvz2native/src/engine/) | Qué llamar y en qué orden (boot, lifecycle, frame) | Solo vía `sym()` |
| [dependencies/](pvz2native/src/dependencies/) | Cómo responder a cada `.so` de Android (libc, libm, GLES, …) | No |
| [dex/](pvz2native/src/dex/) | Cómo responder al lado Java (JNIEnv falso + hooks por clase) | No |
| [diagnostics/](pvz2native/src/diagnostics/) | Cómo interrogar al invitado (watchpoints, muestreo de PC) | Solo vía `sym()` |

Piezas destacadas:

- **JIT:** [dynarmic](dynarmic/) (solo frontend A32). Un JIT por hilo invitado, con *page table* de identidad sobre la memoria del `.so` para velocidad.
- **Dependencias nativas:** un módulo por `.so` de Android que el juego enlaza. La libc está partida por cabecera (`string`, `stdio`, `stdlib`, `time`, `pthread`, `locale`, `ctype`, …), más `libm`, `libz`, `liblog`, `libgles`, `libstdcxx`, `libopensles`, `libdl`. Cada handler es un `void(GuestCall&)` registrado por nombre.
- **Lado Java:** [jni_env.cpp](pvz2native/src/dex/jni_env.cpp) implementa un `JNIEnv`/`JavaVM` falso (objetos, strings, arrays, referencias); [dex/hooks/](pvz2native/src/dex/hooks/) tiene un archivo por clase Java (`AndroidGameApp`, `AndroidSurfaceView`, HTTP, Google Play, Facebook…).
- **Gráficos:** las llamadas GLES del juego se traducen a **OpenGL 2.0 (perfil de compatibilidad)** vía `glad`; SDL provee la ventana y el contexto GL, haciendo el papel del `GLSurfaceView` de Android.
- **Recursos:** un VFS traduce rutas de Android a rutas de PC y lee los assets desde el `.obb` real del juego (formato RSB/RSG).

---

## 🎮 Versiones del juego soportadas

Se detecta la versión por **huella de bytes** en dos funciones nativas conocidas; si no coincide ninguna, el arranque se rechaza en vez de correr a ciegas.

| Versión | OBB típico | Estado |
| --- | --- | --- |
| **1.6.10** (2013) | `main.7.com.ea.game.pvz2_na.obb` | 🟢 Jugable |
| **4.5.2** (2016) | `main.147.com.ea.game.pvz2_row.obb` (a menudo se distribuye / renombra como `data.obb`) | 🟢 Jugable (objetivo principal de este build) |

Añadir una versión nueva = una entrada en `kVersions` de [symbols.cpp](pvz2native/src/game/symbols.cpp). Nada de `runtime/` ni `engine/` cambia entre versiones.

---

## ⚙️ Requisitos

- **(64 bits) Windows o Linux** (el ejecutable es x86_64 estático; no hay build de 32 bits — dynarmic solo trae backends para x86_64/arm64/riscv64).
- **CMake** (se recomienda 3.5+).
- **Windows:** MinGW-w64 (GCC con C++20) **o** Visual Studio con C++ / CMake / Ninja (`compile-ninja.bat`).
- **Linux:** GCC o Clang con soporte C++20.
- **GPU con OpenGL 2.0** o superior.
- Tu propia copia de **`libPVZ2.so`** (armeabi-v7a) y del **`.obb`** correspondiente.
- **Cabeceras de Boost** en `third_party/boost_1_84_0` (como usan los scripts de build).

---

## 🔨 Compilación

### Windows — Visual Studio + Ninja (recomendado)

```bat
compile-ninja.bat
```

Usa `vswhere` para localizar Visual Studio (cualquier edición) y compila Release x64 con Ninja. Salida:

```text
out\build\x64-Release\pvz2native\pvz2.exe
```

### Windows — MinGW

Si `gcc`, `g++` y `make` están en el `PATH`:

```bat
compile.bat
```

> [!NOTE]
> `compile-hardcoded.bat` es un helper MinGW con rutas absolutas de una máquina concreta. Preferí `compile.bat` o `compile-ninja.bat`.

Salida:

```text
build\pvz2native\pvz2.exe
```

### Linux

```bash
./compile.sh
```

Opciones útiles: `--clean`, `--debug`, `--jobs N`, `--no-assets`, `--verbose`, `--help`.

Salida:

```text
build/pvz2native/pvz2
```

`compile.sh` también copia los assets de UI del host a `build/pvz2native/assets/` (splash, ajustes, cursor, logros). En Windows esos archivos van embebidos en el `.exe` vía `windows_resources.rc`; en Linux deben estar junto al binario bajo `assets/`.

### Linux — AppImage

```bash
./compile-appimage.sh
```

Compila con `compile.sh` y empaqueta un AppImage tipo 2. Salida:

```text
build/pvz2native/pvz2.AppImage
```

El AppImage contiene **solo** el binario `pvz2` y `assets/`. Coloca `libPVZ2.so`, `data.obb` y `config.ini` **junto al** `.AppImage` (mismo layout portable que el binario suelto). La primera vez puede descargar `appimagetool` en `tools/` si falta.

---

## ▶️ Ejecución

Coloca los archivos del juego de Android **junto al ejecutable** (no en una subcarpeta `lib/`). Por defecto se buscan `libPVZ2.so` y `data.obb` en esa misma carpeta:

```text
<carpeta con pvz2 / pvz2.exe / pvz2.AppImage>/
├── pvz2(.exe)  o  pvz2.AppImage
├── config.ini                 ← se crea / corrige en el primer arranque
├── libPVZ2.so                 ← obligatorio (armeabi-v7a)
├── data.obb                   ← obligatorio (o apunta [paths] obb a tu nombre de OBB)
└── assets/                    ← binario suelto en Linux: UI/splash/cursor/logros
                                   (Windows: embebido en el exe; AppImage: dentro de la imagen)
```

Puedes conservar el nombre original del OBB y referenciarlo en `config.ini`:

```ini
[paths]
so  = libPVZ2.so
obb = main.147.com.ea.game.pvz2_row.obb
```

Las rutas relativas se resuelven respecto a la carpeta del ejecutable. También se aceptan rutas absolutas.

En el primer arranque el host escribe un `config.ini` documentado. Lo cotidiano se cambia desde la pantalla de ajustes; lo avanzado queda en el archivo / CLI.

---

## 🔧 Configuración (`config.ini`)

Fuente única de verdad para rutas y depuración (las antiguas variables `PVZ2_*` ya no se leen). Extracto:

| Sección | Clave | Para qué sirve |
| --- | --- | --- |
| `[paths]` | `so`, `obb` | Rutas al `.so` y al `.obb` (relativas al ejecutable o absolutas). |
| `[paths]` | `save` | Carpeta de guardado opcional; sin valor, `prefs.dat` junto al exe. |
| `[log]` | `console` | Mostrar consola/log (por defecto `0`). |
| `[log]` | `verbose` / `trace` / `input` | `verbose`: boot + línea de tiempo con número de frame (`[engine]`, `[java]`, `[screen]`, `[census]`, `[state]`, `[quest]`, `[quest-save]`, `[quest-io]`) para cazar una UI muerta (implica `input`; necesita `console=1` o un `logfile`); `trace`: imports; `input`: eventos. |
| `[video]` | `fullscreen`, `display`, `width`, `height`, `fps_limit` | Pantalla / índice de monitor (también en la UI de ajustes). |
| `[game]` | `user_locale`, `currency_*`, `emulate_iap` | Idioma, monedas, IAP gratis (también en la UI). |
| `[game]` | `achievement_notifications` | Toast + SFX al desbloquear (`1` por defecto; `0` solo silencia la UI). |
| `[runtime]` | `use_cdn`, `rewrite_cdn` | CDN emulado activado/desactivado (por defecto activado) y re-copia forzada desde el OBB (también en la UI → Eventos). |
| `[runtime]` | `allow_repeat_quests` | `1` (por defecto): las quests repetibles que ya cobraste vuelven a contar muertes en vez de completarse solas cuando vuelven a salir (la lista de completadas del guardado se limpia al arrancar); `0` = no tocar el guardado. |
| `[shortcuts]` | `bind_*` | Tecla → toque en pantalla (también en la UI). |
| `[board]` / `[gamepad]` / `[gamepad_binds]` | … | Rejilla y binds del mando (también en la UI). |

---

## ⌨️ Controles (host)

| Acción | Control |
| --- | --- |
| Tocar / seleccionar | Clic izquierdo (o **A** del mando) |
| Arrastrar (deslizar) | Clic izquierdo + mover (o stick derecho) |
| Desplazar listas | Rueda del ratón |
| Atrás (menús) | Clic derecho / `Backspace` / Back del mando (igual que Esc: abre configuración, luego la confirmación de salida) |
| Ajustes → confirmar salida | `Esc` (fijo): la primera pulsación abre ajustes, la segunda la confirmación de salida |
| Pantalla completa | `F11` (fijo) |
| Ajustes | Botón Google Play del menú principal |
| Lista de logros | Desde ajustes (**Ver logros**) |
| Atajos / mando | Configurables en ajustes |

La ventana no se puede redimensionar con el ratón (deformaría la imagen); elige el tamaño en ajustes o en `config.ini`, o usa `F11` para pantalla completa.

---

## 📊 Estado del proyecto

| Área | Estado |
| --- | --- |
| Carga del `.so` y arranque del motor | 🟢 |
| Ciclo de vida (init / surface / frames) | 🟢 |
| Carga de recursos (RSB/RSG desde `.obb`) | 🟢 |
| Imports nativos (libc/libm/GLES/…) | 🟢 |
| Renderizado | 🟢 |
| Audio (OpenSL ES → SDL) | 🟢 |
| Entrada (ratón / teclado / mando) | 🟢 |
| Ajustes in-game / confirmación al salir | 🟢 |
| Logros locales (lista + toast) | 🟢 |
| Servicios online | 🔴 No implementado (deliberadamente offline) |

---

## 📁 Estructura del proyecto

```text
PvZ2Native/
├── pvz2native/            ← host / capa de compatibilidad
│   ├── src/
│   │   ├── runtime/       ← JIT (dynarmic), GuestHeap, hilos, sync, RSB
│   │   ├── dependencies/  ← un módulo por .so de Android + VFS
│   │   ├── dex/           ← JNIEnv/JavaVM falso + hooks por clase
│   │   ├── engine/        ← boot / lifecycle / frame
│   │   ├── game/          ← symbols.cpp: direcciones por versión
│   │   ├── diagnostics/
│   │   ├── gfx/, audio/, input/, patch/, elf32/
│   │   ├── config.cpp / config_screen.cpp / achievements.cpp
│   │   └── main.c
│   ├── windows_resources.rc
│   └── include/pvz2native/
├── assets/                ← splash, UI de ajustes, cursor, logros (embebido en Windows; copiado en Linux)
├── dynarmic/  SDL/  glad/  zlib/  stb/  third_party/
├── compile-ninja.bat      ← Windows VS + Ninja
├── compile.bat            ← Windows MinGW (PATH)
├── compile.sh             ← Linux
├── compile-appimage.sh    ← AppImage Linux (`pvz2.AppImage`)
├── CMakeLists.txt
└── README.md
```

---

## 📜 Licencia

Código bajo **licencia MIT** (ver [LICENSE](LICENSE)). El proyecto reutiliza infraestructura de terceros (dynarmic, SDL, zlib, glad, stb), cada uno con su propia licencia.

**Plants vs. Zombies 2**, sus marcas y todos sus recursos pertenecen a **Electronic Arts / PopCap Games**. Este proyecto **no distribuye ni reemplaza** ningún archivo del juego: debes aportar los tuyos, obtenidos legalmente.

## ⚠️ Descargo de responsabilidad

Proyecto independiente, **sin afiliación, respaldo ni patrocinio** de Electronic Arts, PopCap Games ni ninguna entidad relacionada con Plants vs. Zombies. Su fin es exclusivamente **técnico y educativo**: investigación de emulación de binarios ARM, capas de compatibilidad y traducción de APIs.

---

<p align="center">
  <b>PvZ2Native</b> — Ejecutando la <code>libPVZ2.so</code> de Android en PC mediante emulación ARM
</p>
