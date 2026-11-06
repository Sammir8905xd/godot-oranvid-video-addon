# OranVid Player para Godot 4.7

Addon de Windows x86_64 para reproducir archivos `.oranvid` v2 directamente en Godot. Usa el decodificador ORC2/ORA2
del núcleo nativo del proyecto; no ejecuta `oranvid --play`, no necesita VLC y no convierte el archivo a MP4.

## Compilar

El workflow [build-godot-addon.yml](../../.github/workflows/build-godot-addon.yml) compila la extensión para Windows
x86_64 y publica `oranvid-godot-addon-windows-x86_64.zip` como artefacto de GitHub Actions. Ejecuta el workflow
manualmente o súbelo a GitHub y espera a que termine el job.

Extrae la carpeta `oranvid` del ZIP en `res://addons/oranvid/` de tu proyecto Godot. Conserva el subdirectorio
`bin/windows`; contiene la biblioteca nativa compilada.

## Activar y usar

1. Abre **Proyecto → Ajustes del proyecto → Plugins** y activa **OranVid Player**.
2. En el árbol de escena, agrega el nodo **OranVidPlayer** (es un `TextureRect`).
3. Asigna un archivo en `Video Path`, o abre uno desde un script:

```gdscript
@onready var player: OranVidPlayer = $OranVidPlayer

func _ready() -> void:
    player.playback_error.connect(_on_playback_error)
    player.open_file("res://videos/videoPreview_HIGH.vp9.oranvid")

func _on_playback_error(message: String) -> void:
    push_error(message)
```

`autoplay` y `audio_enabled` se pueden configurar desde el Inspector. También están disponibles `play()`, `pause()`,
`seek(seconds)`, `get_position_seconds()`, `get_duration_seconds()` y `close()`. El nodo emite `playback_finished`,
`playback_error(message)` y mantiene el video en su textura.

## Construcción desde código fuente

Requiere Python, SCons, MinGW-w64 y la rama principal de `godot-cpp`. Desde la raíz del proyecto:

```powershell
git clone https://github.com/godotengine/godot-cpp.git
scons -C addons/oranvid platform=windows target=template_release arch=x86_64 use_mingw=yes
```

El build actual del workflow produce la biblioteca para Godot 4.7 sobre Windows x86_64.
