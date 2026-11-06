@tool
extends EditorPlugin

const PLAYER_SCRIPT = preload("res://addons/oranvid/oranvid_player.gd")
const PLAYER_ICON = preload("res://addons/oranvid/icons/oranvid.svg")

func _enter_tree() -> void:
	add_custom_type("OranVidPlayer", "TextureRect", PLAYER_SCRIPT, PLAYER_ICON)

func _exit_tree() -> void:
	remove_custom_type("OranVidPlayer")
