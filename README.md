# OranVid Godot addon

Addon para Godot 4.7 que reproduce archivos `.oranvid` v2 directamente, con el códec ORC2/ORA2 original. No ejecuta
`oranvid --play`, no requiere VLC y no convierte el video a MP4.

El addon está en [`addons/oranvid`](addons/oranvid/README.md). El código del decodificador incluido en `native/core`
permite compilar este repositorio por sí solo.

Para obtener el build de Windows x86_64, ejecuta el workflow **Build Godot OranVid addon** y descarga el artefacto
`oranvid-godot-addon-windows-x86_64`.
