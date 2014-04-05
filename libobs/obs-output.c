/******************************************************************************
    Copyright (C) 2013-2014 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "obs.h"
#include "obs-internal.h"

static inline const struct obs_output_info *find_output(const char *id)
{
	size_t i;
	for (i = 0; i < obs->output_types.num; i++)
		if (strcmp(obs->output_types.array[i].id, id) == 0)
			return obs->output_types.array+i;

	return NULL;
}

static const char *output_signals[] = {
	"void start(ptr output, int errorcode)",
	"void stop(ptr output)",
	NULL
};

static bool init_output_handlers(struct obs_output *output)
{
	output->signals = signal_handler_create();
	if (!output->signals)
		return false;

	output->procs = proc_handler_create();
	if (!output->procs)
		return false;

	signal_handler_add_array(output->signals, output_signals);
	return true;
}

obs_output_t obs_output_create(const char *id, const char *name,
		obs_data_t settings)
{
	const struct obs_output_info *info = find_output(id);
	struct obs_output *output;

	if (!info) {
		blog(LOG_ERROR, "Output '%s' not found", id);
		return NULL;
	}

	output = bzalloc(sizeof(struct obs_output));
	pthread_mutex_init_value(&output->interleaved_mutex);

	if (pthread_mutex_init(&output->interleaved_mutex, NULL) != 0)
		goto fail;
	if (!init_output_handlers(output))
		goto fail;

	output->info     = *info;
	output->video    = obs_video();
	output->audio    = obs_audio();
	output->settings = obs_data_newref(settings);
	if (output->info.defaults)
		output->info.defaults(output->settings);

	output->data = info->create(output->settings, output);
	if (!output->data)
		goto fail;

	output->name = bstrdup(name);

	pthread_mutex_lock(&obs->data.outputs_mutex);
	da_push_back(obs->data.outputs, &output);
	pthread_mutex_unlock(&obs->data.outputs_mutex);

	output->valid = true;

	return output;

fail:
	obs_output_destroy(output);
	return NULL;
}

static inline void free_il_packet(struct il_packet *data)
{
	obs_free_encoder_packet(&data->packet);
}

static inline void free_packets(struct obs_output *output)
{
	for (size_t i = 0; i < output->interleaved_packets.num; i++)
		free_il_packet(output->interleaved_packets.array+i);
	da_free(output->interleaved_packets);
}

void obs_output_destroy(obs_output_t output)
{
	if (output) {
		if (output->valid) {
			if (output->active)
				output->info.stop(output->data);

			pthread_mutex_lock(&obs->data.outputs_mutex);
			da_erase_item(obs->data.outputs, &output);
			pthread_mutex_unlock(&obs->data.outputs_mutex);
		}

		free_packets(output);

		if (output->data)
			output->info.destroy(output->data);

		signal_handler_destroy(output->signals);
		proc_handler_destroy(output->procs);

		obs_data_release(output->settings);
		pthread_mutex_destroy(&output->interleaved_mutex);
		bfree(output->name);
		bfree(output);
	}
}

bool obs_output_start(obs_output_t output)
{
	return (output != NULL) ? output->info.start(output->data) : false;
}

void obs_output_stop(obs_output_t output)
{
	if (output)
		output->info.stop(output->data);
}

bool obs_output_active(obs_output_t output)
{
	return (output != NULL) ? output->active : false;
}

static inline obs_data_t get_defaults(const struct obs_output_info *info)
{
	obs_data_t settings = obs_data_create();
	if (info->defaults)
		info->defaults(settings);
	return settings;
}

obs_data_t obs_output_defaults(const char *id)
{
	const struct obs_output_info *info = find_output(id);
	return (info) ? get_defaults(info) : NULL;
}

obs_properties_t obs_get_output_properties(const char *id, const char *locale)
{
	const struct obs_output_info *info = find_output(id);
	if (info && info->properties) {
		obs_data_t       defaults = get_defaults(info);
		obs_properties_t properties;

		properties = info->properties(locale);
		obs_properties_apply_settings(properties, defaults);
		obs_data_release(defaults);
		return properties;
	}
	return NULL;
}

obs_properties_t obs_output_properties(obs_output_t output, const char *locale)
{
	if (output && output->info.properties) {
		obs_properties_t props;
		props = output->info.properties(locale);
		obs_properties_apply_settings(props, output->settings);
		return props;
	}

	return NULL;
}

void obs_output_update(obs_output_t output, obs_data_t settings)
{
	if (!output) return;

	obs_data_apply(output->settings, settings);

	if (output->info.update)
		output->info.update(output->data, output->settings);
}

obs_data_t obs_output_get_settings(obs_output_t output)
{
	if (!output)
		return NULL;

	obs_data_addref(output->settings);
	return output->settings;
}

bool obs_output_canpause(obs_output_t output)
{
	return output ? (output->info.pause != NULL) : false;
}

void obs_output_pause(obs_output_t output)
{
	if (output && output->info.pause)
		output->info.pause(output->data);
}

signal_handler_t obs_output_signalhandler(obs_output_t output)
{
	return output ? output->signals : NULL;
}

proc_handler_t obs_output_prochandler(obs_output_t output)
{
	return output ? output->procs : NULL;
}

void obs_output_set_media(obs_output_t output, video_t video, audio_t audio)
{
	if (!output)
		return;

	output->video = video;
	output->audio = audio;
}

video_t obs_output_video(obs_output_t output)
{
	return output ? output->video : NULL;
}

audio_t obs_output_audio(obs_output_t output)
{
	return output ? output->audio : NULL;
}

void obs_output_remove_encoder(struct obs_output *output,
		struct obs_encoder *encoder)
{
	if (!output) return;

	if (output->video_encoder == encoder)
		output->video_encoder = NULL;
	else if (output->audio_encoder == encoder)
		output->audio_encoder = NULL;
}

void obs_output_set_video_encoder(obs_output_t output, obs_encoder_t encoder)
{
	if (!output) return;
	if (output->video_encoder == encoder) return;
	if (encoder && encoder->info.type != OBS_ENCODER_VIDEO) return;

	obs_encoder_remove_output(encoder, output);
	obs_encoder_add_output(encoder, output);
	output->video_encoder = encoder;
}

void obs_output_set_audio_encoder(obs_output_t output, obs_encoder_t encoder)
{
	if (!output) return;
	if (output->audio_encoder == encoder) return;
	if (encoder && encoder->info.type != OBS_ENCODER_AUDIO) return;

	obs_encoder_remove_output(encoder, output);
	obs_encoder_add_output(encoder, output);
	output->audio_encoder = encoder;
}

obs_encoder_t obs_output_get_video_encoder(obs_output_t output)
{
	return output ? output->video_encoder : NULL;
}

obs_encoder_t obs_output_get_audio_encoder(obs_output_t output)
{
	return output ? output->audio_encoder : NULL;
}

void obs_output_set_video_conversion(obs_output_t output,
		const struct video_scale_info *conversion)
{
	if (!output || !conversion) return;

	output->video_conversion = *conversion;
	output->video_conversion_set = true;
}

void obs_output_set_audio_conversion(obs_output_t output,
		const struct audio_convert_info *conversion)
{
	if (!output || !conversion) return;

	output->audio_conversion = *conversion;
	output->audio_conversion_set = true;
}

static bool can_begin_data_capture(struct obs_output *output, bool encoded,
		bool has_video, bool has_audio)
{
	if (has_video) {
		if (encoded) {
			if (!output->video_encoder)
				return false;
		} else {
			if (!output->video)
				return false;
		}
	}

	if (has_audio) {
		if (encoded) {
			if (!output->audio_encoder)
				return false;
		} else {
			if (!output->audio)
				return false;
		}
	}

	return true;
}

static inline struct video_scale_info *get_video_conversion(
		struct obs_output *output)
{
	return output->video_conversion_set ? &output->video_conversion : NULL;
}

static inline struct audio_convert_info *get_audio_conversion(
		struct obs_output *output)
{
	return output->audio_conversion_set ? &output->audio_conversion : NULL;
}

#define MICROSECOND_DEN 1000000

static inline int64_t convert_packet_dts(struct encoder_packet *packet)
{
	return packet->dts * MICROSECOND_DEN / packet->timebase_den;
}

static bool prepare_interleaved_packet(struct obs_output *output,
		struct il_packet *out, struct encoder_packet *packet)
{
	int64_t offset;

	out->input_ts_us = convert_packet_dts(packet);

	/* audio and video need to start at timestamp 0, and the encoders
	 * may not currently be at 0 when we get data.  so, we store the
	 * current dts as offset and subtract that value from the dts/pts
	 * of the output packet. */
	if (packet->type == OBS_ENCODER_VIDEO) {
		if (!output->received_video) {
			output->first_video_ts = out->input_ts_us;
			output->video_offset   = packet->dts;
			output->received_video = true;
		}

		offset = output->video_offset;
	} else{
		/* don't accept audio that's before the first video timestamp */
		if (!output->received_video ||
		    out->input_ts_us < output->first_video_ts)
			return false;

		if (!output->received_audio) {
			output->audio_offset   = packet->dts;
			output->received_audio = true;
		}

		offset = output->audio_offset;
	}

	obs_duplicate_encoder_packet(&out->packet, packet);
	out->packet.dts -= offset;
	out->packet.pts -= offset;
	out->output_ts_us = convert_packet_dts(&out->packet);
	return true;
}

static inline void send_interleaved(struct obs_output *output)
{
	struct il_packet out = output->interleaved_packets.array[0];
	da_erase(output->interleaved_packets, 0);

	output->info.encoded_packet(output->data, &out.packet);
	free_il_packet(&out);
}

static void interleave_packets(void *data, struct encoder_packet *packet)
{
	struct obs_output *output = data;
	struct il_packet  out;
	size_t            idx;

	pthread_mutex_lock(&output->interleaved_mutex);

	if (!prepare_interleaved_packet(output, &out, packet)) {

		for (idx = 0; idx < output->interleaved_packets.num; idx++) {
			struct il_packet *cur_packet;
			cur_packet = output->interleaved_packets.array + idx;

			if (out.output_ts_us < cur_packet->output_ts_us)
				break;
		}

		da_insert(output->interleaved_packets, idx, &out);

		/* when both video and audio have been received, we're ready
		 * to start sending out packets (one at a time) */
		if (output->received_audio && output->received_video)
			send_interleaved(output);
	}

	pthread_mutex_unlock(&output->interleaved_mutex);
}

static void hook_data_capture(struct obs_output *output, bool encoded,
		bool has_video, bool has_audio)
{
	void (*encoded_callback)(void *data, struct encoder_packet *packet);
	void *param;

	if (encoded) {
		output->received_video = false;
		output->received_video = false;

		encoded_callback = (has_video && has_audio) ?
			interleave_packets : output->info.encoded_packet;
		param = (has_video && has_audio) ? output : output->data;

		if (has_video)
			obs_encoder_start(output->video_encoder,
					encoded_callback, param);
		if (has_audio)
			obs_encoder_start(output->audio_encoder,
					encoded_callback, param);
	} else {
		if (has_video)
			video_output_connect(output->video,
					get_video_conversion(output),
					output->info.raw_video,
					output->data);
		if (has_audio)
			audio_output_connect(output->audio,
					get_audio_conversion(output),
					output->info.raw_audio,
					output->data);
	}
}

static inline void signal_start(struct obs_output *output, int code)
{
	struct calldata params = {0};
	calldata_setint(&params, "code", code);
	calldata_setptr(&params, "output", output);
	signal_handler_signal(output->signals, "start", &params);
	calldata_free(&params);
}

static inline void signal_stop(struct obs_output *output)
{
	struct calldata params = {0};
	calldata_setptr(&params, "output", output);
	signal_handler_signal(output->signals, "stop", &params);
	calldata_free(&params);
}

static inline void convert_flags(struct obs_output *output, uint32_t flags,
		bool *encoded, bool *has_video, bool *has_audio)
{
	*encoded = (output->info.flags & OBS_OUTPUT_ENCODED) != 0;
	if (!flags)
		flags = output->info.flags;
	else
		flags &= output->info.flags;

	*has_video = (flags & OBS_OUTPUT_VIDEO) != 0;
	*has_audio = (flags & OBS_OUTPUT_AUDIO) != 0;
}

bool obs_output_can_begin_data_capture(obs_output_t output, uint32_t flags)
{
	bool encoded, has_video, has_audio;

	if (!output) return false;
	if (output->active) return false;

	convert_flags(output, flags, &encoded, &has_video, &has_audio);

	return can_begin_data_capture(output, encoded, has_video, has_audio);
}

bool obs_output_begin_data_capture(obs_output_t output, uint32_t flags)
{
	bool encoded, has_video, has_audio;

	if (!output) return false;
	if (output->active) return false;

	convert_flags(output, flags, &encoded, &has_video, &has_audio);

	if (!can_begin_data_capture(output, encoded, has_video, has_audio))
		return false;

	hook_data_capture(output, encoded, has_video, has_audio);
	output->active = true;
	signal_start(output, OBS_OUTPUT_SUCCESS);
	return true;
}

void obs_output_end_data_capture(obs_output_t output)
{
	bool encoded, has_video, has_audio;
	void (*encoded_callback)(void *data, struct encoder_packet *packet);
	void *param;

	if (!output) return;
	if (!output->active) return;

	convert_flags(output, 0, &encoded, &has_video, &has_audio);

	if (encoded) {
		encoded_callback = (has_video && has_audio) ?
			interleave_packets : output->info.encoded_packet;
		param = (has_video && has_audio) ? output : output->data;

		if (has_video)
			obs_encoder_stop(output->video_encoder,
					encoded_callback, param);
		if (has_audio)
			obs_encoder_stop(output->audio_encoder,
					encoded_callback, param);
	} else {
		if (has_video)
			video_output_disconnect(output->video,
					output->info.raw_video,
					output->data);
		if (has_audio)
			audio_output_disconnect(output->audio,
					output->info.raw_audio,
					output->data);
	}

	output->active = false;
	signal_stop(output);
}

void obs_output_signal_start_fail(obs_output_t output, int code)
{
	signal_start(output, code);
}
