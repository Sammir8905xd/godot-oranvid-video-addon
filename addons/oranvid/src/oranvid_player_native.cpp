#include "oranvid_player_native.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

void OranVidNative::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open_file", "path"), &OranVidNative::open_file);
	ClassDB::bind_method(D_METHOD("close_file"), &OranVidNative::close_file);
	ClassDB::bind_method(D_METHOD("play"), &OranVidNative::play);
	ClassDB::bind_method(D_METHOD("pause"), &OranVidNative::pause);
	ClassDB::bind_method(D_METHOD("seek", "seconds"), &OranVidNative::seek);
	ClassDB::bind_method(D_METHOD("is_open"), &OranVidNative::is_open);
	ClassDB::bind_method(D_METHOD("is_playing"), &OranVidNative::is_playing);
	ClassDB::bind_method(D_METHOD("get_position"), &OranVidNative::get_position);
	ClassDB::bind_method(D_METHOD("get_duration"), &OranVidNative::get_duration);
	ClassDB::bind_method(D_METHOD("get_video_width"), &OranVidNative::get_video_width);
	ClassDB::bind_method(D_METHOD("get_video_height"), &OranVidNative::get_video_height);
	ClassDB::bind_method(D_METHOD("get_audio_sample_rate"), &OranVidNative::get_audio_sample_rate);
	ClassDB::bind_method(D_METHOD("get_audio_channels"), &OranVidNative::get_audio_channels);
	ClassDB::bind_method(D_METHOD("get_last_error"), &OranVidNative::get_last_error);

	ADD_SIGNAL(MethodInfo("frame_ready",
			PropertyInfo(Variant::OBJECT, "image", PROPERTY_HINT_RESOURCE_TYPE, "Image"),
			PropertyInfo(Variant::INT, "frame_index")));
	ADD_SIGNAL(MethodInfo("audio_ready",
			PropertyInfo(Variant::PACKED_VECTOR2_ARRAY, "samples"),
			PropertyInfo(Variant::INT, "sample_rate")));
	ADD_SIGNAL(MethodInfo("playback_finished"));
	ADD_SIGNAL(MethodInfo("playback_error", PropertyInfo(Variant::STRING, "message")));
}

void OranVidNative::_notification(int what) {
	if (what == NOTIFICATION_PREDELETE) {
		close_file();
	}
}

OranVidNative::~OranVidNative() {
	close_file();
}

int64_t OranVidNative::read_file(void *context, void *buffer, size_t length) {
	OranVidNative *self = static_cast<OranVidNative *>(context);
	if (self->file.is_null() || length > static_cast<size_t>(INT64_MAX)) {
		return -1;
	}
	PackedByteArray bytes = self->file->get_buffer(static_cast<int64_t>(length));
	const int64_t bytes_read = bytes.size();
	if (bytes_read > 0) {
		std::memcpy(buffer, bytes.ptr(), static_cast<size_t>(bytes_read));
	}
	return bytes_read;
}

int OranVidNative::seek_file(void *context, int64_t offset) {
	OranVidNative *self = static_cast<OranVidNative *>(context);
	if (self->file.is_null() || offset < 0) {
		return -1;
	}
	self->file->seek(static_cast<uint64_t>(offset));
	return self->file->get_position() == static_cast<uint64_t>(offset) ? 0 : -1;
}

void OranVidNative::release_pending_chunk() {
	if (has_pending_chunk) {
		orn_chunk_free(&pending_chunk);
		pending_chunk = {};
		has_pending_chunk = false;
	}
}

bool OranVidNative::reset_decoder() {
	if (decoder != nullptr) {
		orn_vdec_free(decoder);
		decoder = nullptr;
	}
	decoder = orn_vdec_new(
			static_cast<int>(header.width),
			static_cast<int>(header.height),
			header.quant_step,
			std::min(8, orn_cpu_count()));
	if (decoder == nullptr) {
		last_error = "No se pudo crear el decodificador ORC2.";
		return false;
	}
	return true;
}

void OranVidNative::fail(const String &message) {
	last_error = message;
	playing = false;
	set_process(false);
	emit_signal("playback_error", last_error);
}

bool OranVidNative::open_file(const String &path) {
	close_file();
	file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		last_error = String("No se pudo abrir el archivo: ") + path;
		return false;
	}

	orn_io io = {
		this,
		&OranVidNative::read_file,
		&OranVidNative::seek_file,
		static_cast<int64_t>(file->get_length())
	};
	reader = orn_reader_open(io, &header);
	if (reader == nullptr) {
		close_file();
		last_error = "Archivo .oranvid inválido o versión no compatible (se requiere v2).";
		return false;
	}
	if (!header.has_video || header.fps_num == 0 || header.fps_den == 0) {
		close_file();
		last_error = "El archivo no contiene video válido.";
		return false;
	}
	if (!reset_decoder()) {
		close_file();
		return false;
	}

	opened = true;
	playing = false;
	at_end = false;
	target_frame = 0;
	position_seconds = 0.0;
	requested_position = 0.0;
	last_error = String();
	set_process(false);
	return true;
}

void OranVidNative::close_file() {
	playing = false;
	set_process(false);
	release_pending_chunk();
	if (decoder != nullptr) {
		orn_vdec_free(decoder);
		decoder = nullptr;
	}
	if (reader != nullptr) {
		orn_reader_close(reader);
		reader = nullptr;
	}
	file.unref();
	header = {};
	opened = false;
	at_end = false;
	target_frame = 0;
	position_seconds = 0.0;
	requested_position = 0.0;
}

double OranVidNative::get_duration() const {
	if (header.fps_num == 0 || header.fps_den == 0) {
		return 0.0;
	}
	return static_cast<double>(header.frame_count) * header.fps_den / header.fps_num;
}

void OranVidNative::play() {
	if (!opened) {
		return;
	}
	if (at_end && !seek(0.0)) {
		return;
	}
	playing = true;
	set_process(true);
}

void OranVidNative::pause() {
	playing = false;
	set_process(false);
}

bool OranVidNative::seek(double seconds) {
	if (!opened || !std::isfinite(seconds) || seconds < 0.0) {
		last_error = "La posición solicitada no es válida.";
		return false;
	}

	requested_position = std::min(seconds, get_duration());
	target_frame = static_cast<int64_t>(requested_position * header.fps_num / header.fps_den);
	target_frame = std::clamp<int64_t>(target_frame, 0, std::max<int64_t>(0, static_cast<int64_t>(header.frame_count) - 1));

	int64_t offset = -1;
	const int index_count = orn_reader_index_count(reader);
	for (int i = 0; i < index_count; ++i) {
		int64_t frame = 0;
		int64_t candidate = 0;
		orn_reader_index_get(reader, i, &frame, &candidate);
		if (frame > target_frame) {
			break;
		}
		offset = candidate;
	}
	if (offset >= 0) {
		if (orn_reader_seek(reader, offset) != 0) {
			last_error = "No se pudo buscar en el archivo.";
			return false;
		}
	} else {
		orn_reader_rewind(reader);
	}

	release_pending_chunk();
	if (!reset_decoder()) {
		return false;
	}
	position_seconds = requested_position;
	at_end = false;
	last_error = String();
	return true;
}

Ref<Image> OranVidNative::make_image(const uint8_t *i420) const {
	const int width = static_cast<int>(header.width);
	const int height = static_cast<int>(header.height);
	const int y_size = width * height;
	const int chroma_width = width / 2;
	const bool full_range = std::strcmp(orn_reader_meta(reader, "range") ? orn_reader_meta(reader, "range") : "", "pc") == 0;
	const char *matrix_name = orn_reader_meta(reader, "colormatrix");

	int rv = 22970, gu = -5638, gv = -11700, bu = 29032;
	if (matrix_name != nullptr && std::strcmp(matrix_name, "bt709") == 0) {
		rv = 25802; gu = -3069; gv = -7668; bu = 30382;
	} else if (matrix_name != nullptr && std::strcmp(matrix_name, "bt2020nc") == 0) {
		rv = 24160; gu = -2696; gv = -9359; bu = 30825;
	}

	PackedByteArray rgb;
	rgb.resize(y_size * 3);
	uint8_t *dst = rgb.ptrw();
	const uint8_t *u_plane = i420 + y_size;
	const uint8_t *v_plane = u_plane + y_size / 4;

	for (int y = 0; y < height; ++y) {
		const int y_row = y * width;
		const int chroma_row = (y / 2) * chroma_width;
		for (int x = 0; x < width; ++x) {
			const int chroma_index = chroma_row + x / 2;
			const int u = static_cast<int>(u_plane[chroma_index]) - 128;
			const int v = static_cast<int>(v_plane[chroma_index]) - 128;
			const int luma = i420[y_row + x];
			const int yy = full_range ? luma << 14 : (luma - 16) * 19077;
			const int pixel = (y_row + x) * 3;
			dst[pixel] = static_cast<uint8_t>(std::clamp((yy + rv * v + 8192) >> 14, 0, 255));
			dst[pixel + 1] = static_cast<uint8_t>(std::clamp((yy + gu * u + gv * v + 8192) >> 14, 0, 255));
			dst[pixel + 2] = static_cast<uint8_t>(std::clamp((yy + bu * u + 8192) >> 14, 0, 255));
		}
	}
	return Image::create_from_data(width, height, false, Image::FORMAT_RGB8, rgb);
}

void OranVidNative::process_audio_chunk(const orn_chunk &chunk) {
	if (!header.has_audio || header.sample_rate == 0 || header.channels < 1 || header.channels > 2) {
		return;
	}

	int16_t *pcm = nullptr;
	int frames = 0;
	if (orn_audio_decode(header.channels, header.audio_shift, chunk.data, chunk.len, &pcm, &frames) != 0) {
		fail("No se pudo decodificar un bloque de audio ORA2.");
		return;
	}

	const int64_t target_sample = static_cast<int64_t>(requested_position * header.sample_rate);
	const int skip_frames = static_cast<int>(std::clamp<int64_t>(target_sample - chunk.pts, 0, frames));
	PackedVector2Array samples;
	samples.resize(frames - skip_frames);
	Vector2 *out = samples.ptrw();
	for (int i = skip_frames; i < frames; ++i) {
		const float left = static_cast<float>(pcm[i * header.channels]) / 32768.0f;
		const float right = header.channels == 2 ? static_cast<float>(pcm[i * header.channels + 1]) / 32768.0f : left;
		out[i - skip_frames] = Vector2(left, right);
	}
	std::free(pcm);

	if (!samples.is_empty()) {
		emit_signal("audio_ready", samples, static_cast<int32_t>(header.sample_rate));
	}
}

void OranVidNative::_process(double delta) {
	if (!playing || !opened || reader == nullptr || decoder == nullptr) {
		return;
	}
	position_seconds = std::min(position_seconds + delta, get_duration());

	int processed_frames = 0;
	while (processed_frames < 4 && playing) {
		orn_chunk chunk = {};
		if (has_pending_chunk) {
			chunk = pending_chunk;
			pending_chunk = {};
			has_pending_chunk = false;
		} else {
			const int status = orn_reader_next(reader, &chunk, 0);
			if (status == 0) {
				at_end = true;
				playing = false;
				set_process(false);
				emit_signal("playback_finished");
				return;
			}
			if (status < 0) {
				fail("El archivo está truncado o un chunk tiene un CRC incorrecto.");
				return;
			}
		}

		if (chunk.type == ORN_CH_INDEX) {
			orn_chunk_free(&chunk);
			at_end = true;
			playing = false;
			set_process(false);
			emit_signal("playback_finished");
			return;
		}
		if (chunk.type == ORN_CH_AUDIO) {
			if (header.sample_rate > 0) {
				const double audio_time = static_cast<double>(chunk.pts) / header.sample_rate;
				if (audio_time > position_seconds) {
					pending_chunk = chunk;
					has_pending_chunk = true;
					return;
				}
			}
			process_audio_chunk(chunk);
			orn_chunk_free(&chunk);
			if (!playing) {
				return;
			}
			continue;
		}
		if (chunk.type != ORN_CH_VKEY && chunk.type != ORN_CH_VDELTA) {
			orn_chunk_free(&chunk);
			continue;
		}

		const double frame_time = static_cast<double>(chunk.pts) * header.fps_den / header.fps_num;
		if (frame_time > position_seconds) {
			pending_chunk = chunk;
			has_pending_chunk = true;
			return;
		}

		const bool is_key = chunk.type == ORN_CH_VKEY;
		const uint8_t *i420 = nullptr;
		if (orn_vdec_decode(decoder, is_key, chunk.data, chunk.len, &i420) != 0) {
			orn_chunk_free(&chunk);
			fail("No se pudo decodificar un frame ORC2.");
			return;
		}
		if (chunk.pts >= target_frame) {
			Ref<Image> image = make_image(i420);
			if (image.is_null()) {
				orn_chunk_free(&chunk);
				fail("Godot no pudo crear la imagen del frame.");
				return;
			}
			emit_signal("frame_ready", image, chunk.pts);
			++processed_frames;
		}
		orn_chunk_free(&chunk);
	}
}

} // namespace godot
