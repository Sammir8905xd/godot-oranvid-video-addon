#ifndef ORANVID_PLAYER_NATIVE_H
#define ORANVID_PLAYER_NATIVE_H

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "../../../native/core/oranvid.h"

namespace godot {

class OranVidNative : public Node {
	GDCLASS(OranVidNative, Node)

	Ref<FileAccess> file;
	orn_reader *reader = nullptr;
	orn_vdec *decoder = nullptr;
	orn_header header = {};
	orn_chunk pending_chunk = {};
	bool has_pending_chunk = false;
	bool opened = false;
	bool playing = false;
	bool at_end = false;
	int64_t target_frame = 0;
	double position_seconds = 0.0;
	double requested_position = 0.0;
	String last_error;

	static int64_t read_file(void *context, void *buffer, size_t length);
	static int seek_file(void *context, int64_t offset);
	void release_pending_chunk();
	bool reset_decoder();
	Ref<Image> make_image(const uint8_t *i420) const;
	void process_audio_chunk(const orn_chunk &chunk);
	void fail(const String &message);

protected:
	static void _bind_methods();
	void _notification(int what);

public:
	~OranVidNative() override;

	bool open_file(const String &path);
	void close_file();
	void play();
	void pause();
	bool seek(double seconds);

	bool is_open() const { return opened; }
	bool is_playing() const { return playing; }
	double get_position() const { return position_seconds; }
	double get_duration() const;
	int32_t get_video_width() const { return static_cast<int32_t>(header.width); }
	int32_t get_video_height() const { return static_cast<int32_t>(header.height); }
	int32_t get_audio_sample_rate() const { return static_cast<int32_t>(header.sample_rate); }
	int32_t get_audio_channels() const { return static_cast<int32_t>(header.channels); }
	String get_last_error() const { return last_error; }

	void _process(double delta) override;
};

} // namespace godot

#endif
