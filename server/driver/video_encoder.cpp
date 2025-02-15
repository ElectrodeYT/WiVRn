/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Include first because of incompatibility between Eigen and X includes
#include "wivrn_session.h"

#include "video_encoder.h"
#include "util/u_logging.h"

#include <string>

#include "wivrn_config.h"

#ifdef WIVRN_HAVE_CUDA
#include "video_encoder_nvenc.h"
#endif
#ifdef WIVRN_HAVE_FFMPEG
#include "ffmpeg/VideoEncoderVA.h"
#endif
#ifdef WIVRN_HAVE_X264
#include "video_encoder_x264.h"
#endif

namespace xrt::drivers::wivrn
{

std::unique_ptr<VideoEncoder> VideoEncoder::Create(
        vk_bundle * vk,
        vk_cmd_pool & pool,
        encoder_settings & settings,
        uint8_t stream_idx,
        int input_width,
        int input_height,
        float fps)
{
	using namespace std::string_literals;
	std::unique_ptr<VideoEncoder> res;
#ifdef WIVRN_HAVE_X264
	if (settings.encoder_name == encoder_x264)
	{
		res = std::make_unique<VideoEncoderX264>(vk, pool, settings, input_width, input_height, fps);
	}
#endif
#ifdef WIVRN_HAVE_CUDA
	if (settings.encoder_name == encoder_nvenc)
	{
		res = std::make_unique<VideoEncoderNvenc>(vk, settings, fps);
	}
#endif
#ifdef WIVRN_HAVE_FFMPEG
	if (settings.encoder_name == encoder_vaapi)
	{
		res = std::make_unique<VideoEncoderVA>(vk, settings, fps);
	}
#endif
	if (res)
	{
		res->stream_idx = stream_idx;
	}
	else
	{
		U_LOG_E("No video encoder %s", settings.encoder_name.c_str());
	}
	return res;
}

void VideoEncoder::Encode(wivrn_session & cnx,
                          const to_headset::video_stream_data_shard::view_info_t & view_info,
                          uint64_t frame_index,
                          int index,
                          bool idr)
{
	this->cnx = &cnx;
	auto target_timestamp = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(view_info.display_time));
	cnx.dump_time("encode_begin", frame_index, os_monotonic_get_ns(), stream_idx);

	// Prepare the video shard template
	shard.stream_item_idx = stream_idx;
	shard.frame_idx = frame_index;
	shard.shard_idx = 0;
	shard.view_info = view_info;

	Encode(index, idr, target_timestamp);
	cnx.dump_time("encode_end", frame_index, os_monotonic_get_ns(), stream_idx);
}

void VideoEncoder::SendData(std::span<uint8_t> data, bool end_of_frame)
{
	std::lock_guard lock(mutex);
#if 0
	std::ofstream debug("/tmp/video_dump-" + std::to_string(stream_idx), std::ios::app);
	debug.write((char*)data.data(), data.size());
#endif
	if (shard.shard_idx == 0)
		cnx->dump_time("send_start", shard.frame_idx, os_monotonic_get_ns(), stream_idx);

	shard.flags = to_headset::video_stream_data_shard::start_of_slice;
	auto begin = data.begin();
	auto end = data.end();
	while (begin != end)
	{
		const size_t view_info_size = sizeof(to_headset::video_stream_data_shard::view_info_t);
		const size_t max_payload_size = to_headset::video_stream_data_shard::max_payload_size - (shard.view_info ? view_info_size : 0);
		auto next = std::min(end, begin + max_payload_size);
		if (next == end)
		{
			shard.flags |= to_headset::video_stream_data_shard::end_of_slice;
			if (end_of_frame)
				shard.flags |= to_headset::video_stream_data_shard::end_of_frame;
		}
		shard.payload = {begin, next};
		cnx->send_stream(shard);
		++shard.shard_idx;
		shard.flags = 0;
		shard.view_info.reset();
		begin = next;
	}
}

} // namespace xrt::drivers::wivrn
