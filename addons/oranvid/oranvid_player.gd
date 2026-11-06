@tool
extends TextureRect
class_name OranVidPlayer

signal playback_finished
signal playback_error(message: String)

@export var autoplay := true
@export var audio_enabled := true
@export_file("*.oranvid") var video_path := "":
	set(value):
		video_path = value
		if is_node_ready() and not video_path.is_empty():
			open_file(video_path)

var _native: OranVidNative
var _audio_player: AudioStreamPlayer
var _audio_stream: AudioStreamGenerator
var _texture: ImageTexture

func _ready() -> void:
	expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	mouse_filter = Control.MOUSE_FILTER_STOP

	if Engine.is_editor_hint():
		return

	_native = OranVidNative.new()
	_native.name = "OranVidDecoder"
	add_child(_native)
	_native.frame_ready.connect(_on_frame_ready)
	_native.audio_ready.connect(_on_audio_ready)
	_native.playback_finished.connect(_on_playback_finished)
	_native.playback_error.connect(_on_playback_error)

	_audio_player = AudioStreamPlayer.new()
	_audio_player.name = "OranVidAudio"
	add_child(_audio_player)

	if not video_path.is_empty():
		open_file(video_path)

func open_file(path: String) -> bool:
	if Engine.is_editor_hint():
		return false
	if _native == null:
		push_error("OranVidPlayer debe estar dentro de una escena antes de abrir un archivo.")
		return false
	_audio_player.stop()
	if not _native.open_file(path):
		_on_playback_error(_native.get_last_error())
		return false

	_texture = null
	texture = null
	_configure_audio()
	if autoplay:
		play()
	return true

func play() -> void:
	if _native == null or not _native.is_open():
		return
	if audio_enabled and _audio_stream != null:
		if _audio_player.playing:
			_audio_player.stream_paused = false
		else:
			_audio_player.play()
	_native.play()

func pause() -> void:
	if _native == null:
		return
	_native.pause()
	_audio_player.stream_paused = true

func seek(seconds: float) -> bool:
	if _native == null or not _native.seek(seconds):
		if _native != null:
			_on_playback_error(_native.get_last_error())
		return false
	if _audio_player.playing:
		_audio_player.stop()
		if audio_enabled and _audio_stream != null:
			_audio_player.play()
	return true

func get_position_seconds() -> float:
	return _native.get_position() if _native != null else 0.0

func get_duration_seconds() -> float:
	return _native.get_duration() if _native != null else 0.0

func close() -> void:
	if _native != null:
		_native.close_file()
	if _audio_player != null:
		_audio_player.stop()
	texture = null
	_texture = null

func _configure_audio() -> void:
	if not audio_enabled or _native.get_audio_sample_rate() <= 0:
		_audio_player.stream = null
		_audio_stream = null
		return
	_audio_stream = AudioStreamGenerator.new()
	_audio_stream.mix_rate = _native.get_audio_sample_rate()
	_audio_stream.buffer_length = 2.0
	_audio_player.stream = _audio_stream

func _on_frame_ready(image: Image, _frame_index: int) -> void:
	if _texture == null or _texture.get_width() != image.get_width() or _texture.get_height() != image.get_height():
		_texture = ImageTexture.create_from_image(image)
		texture = _texture
	else:
		_texture.update(image)

func _on_audio_ready(samples: PackedVector2Array, _sample_rate: int) -> void:
	if not audio_enabled or not _audio_player.playing:
		return
	var playback := _audio_player.get_stream_playback() as AudioStreamGeneratorPlayback
	if playback == null:
		return
	if playback.get_frames_available() < samples.size():
		push_warning("El audio OranVid va por delante del búfer de Godot; se descartó un bloque para mantener la sincronización.")
		return
	playback.push_buffer(samples)

func _on_playback_finished() -> void:
	playback_finished.emit()

func _on_playback_error(message: String) -> void:
	_audio_player.stop()
	push_error(message)
	playback_error.emit(message)
