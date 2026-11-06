#include "register_types.h"

#include "oranvid_player_native.h"

#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_oranvid_module(ModuleInitializationLevel level) {
	if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(OranVidNative);
	}
}

void uninitialize_oranvid_module(ModuleInitializationLevel level) {
	(void)level;
}

extern "C" {

GDExtensionBool GDE_EXPORT oranvid_library_init(
		GDExtensionInterfaceGetProcAddress get_proc_address,
		GDExtensionClassLibraryPtr library,
		GDExtensionInitialization *initialization) {
	GDExtensionBinding::InitObject init_object(get_proc_address, library, initialization);
	init_object.register_initializer(initialize_oranvid_module);
	init_object.register_terminator(uninitialize_oranvid_module);
	init_object.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_object.init();
}

}
