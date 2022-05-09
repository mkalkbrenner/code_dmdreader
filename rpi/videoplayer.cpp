
/*
 * Copyright (c) 2017 Jun Zhao
 * Copyright (c) 2017 Kaixuan Liu
 *
 * HW Acceleration API (video decoding) decode sample
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

 /**
  * @file
  * HW-Accelerated decoding example.
  *
  * @example hw_decode.c
  * This example shows how to do HW-accelerated decoding with output
  * frames from the HW video surfaces.
  */

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <boost/log/trivial.hpp>

extern "C" {
#include "drmprime_out.h"
}

#include "drmhelper.h"

using namespace std;

static enum AVPixelFormat hw_pix_fmt;
static FILE* output_file = NULL;
static long frames = 0;

static AVFilterContext* buffersink_ctx = NULL;
static AVFilterContext* buffersrc_ctx = NULL;
static AVFilterGraph* filter_graph = NULL;


static int hw_decoder_init(AVCodecContext* ctx, const enum AVHWDeviceType type)
{
	int err = 0;

	ctx->hw_frames_ctx = NULL;
	// ctx->hw_device_ctx gets freed when we call avcodec_free_context
	if ((err = av_hwdevice_ctx_create(&ctx->hw_device_ctx, type,
		NULL, NULL, 0)) < 0) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] failed to create specified HW device.";
		return err;
	}

	return err;
}

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
	const enum AVPixelFormat* pix_fmts)
{
	const enum AVPixelFormat* p;

	for (p = pix_fmts; *p != -1; p++) {
		if (*p == hw_pix_fmt)
			return *p;
	}

	BOOST_LOG_TRIVIAL(error) << "[videoplayer] failed to get HW surface format.";
	return AV_PIX_FMT_NONE;
}

static int decode_write(AVCodecContext* const avctx,
	drmprime_out_env_t* const dpo,
	AVPacket* packet)
{
	AVFrame* frame = NULL, * sw_frame = NULL;
	uint8_t* buffer = NULL;
	int size;
	int ret = 0;
	unsigned int i;

	ret = avcodec_send_packet(avctx, packet);
	if (ret < 0) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] error during decoding";
		return ret;
	}

	for (;;) {
		if (!(frame = av_frame_alloc()) || !(sw_frame = av_frame_alloc())) {
			BOOST_LOG_TRIVIAL(error) << "[videoplayer] can not alloc frame";
			ret = AVERROR(ENOMEM);
			goto fail;
		}

		ret = avcodec_receive_frame(avctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			av_frame_free(&frame);
			av_frame_free(&sw_frame);
			return 0;
		}
		else if (ret < 0) {
			BOOST_LOG_TRIVIAL(error) << "[videoplayer] error while decoding";
			goto fail;
		}

		// push the decoded frame into the filtergraph if it exists
		if (filter_graph != NULL &&
			(ret = av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
			BOOST_LOG_TRIVIAL(error) << "[videoplayer] error while feeding the filtergraph";
			goto fail;
		}

		do {
			if (filter_graph != NULL) {
				av_frame_unref(frame);
				ret = av_buffersink_get_frame(buffersink_ctx, frame);
				if (ret == AVERROR(EAGAIN)) {
					ret = 0;
					break;
				}
				if (ret < 0) {
					if (ret != AVERROR_EOF) {
						BOOST_LOG_TRIVIAL(error) << "[videoplayer] failed to get frame: " << ret;
					}
					goto fail;
				}
			}

			drmprime_out_display(dpo, frame);


		} while (buffersink_ctx != NULL);  // Loop if we have a filter to drain

		if (frames == 0 || --frames == 0)
			ret = -1;

	fail:
		av_frame_free(&frame);
		av_frame_free(&sw_frame);
		av_freep(&buffer);
		if (ret < 0)
			return ret;
	}
	return 0;
}


// Copied almost directly from ffmpeg filtering_video.c example
static int init_filters(const AVStream* const stream,
	const AVCodecContext* const dec_ctx,
	const char* const filters_descr)
{
	char args[512];
	int ret = 0;
	const AVFilter* buffersrc = avfilter_get_by_name("buffer");
	const AVFilter* buffersink = avfilter_get_by_name("buffersink");
	AVFilterInOut* outputs = avfilter_inout_alloc();
	AVFilterInOut* inputs = avfilter_inout_alloc();
	AVRational time_base = stream->time_base;
	enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE };

	filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !filter_graph) {
		ret = AVERROR(ENOMEM);
		goto end;
	}

	/* buffer video source: the decoded frames from the decoder will be inserted here. */
	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
		time_base.num, time_base.den,
		dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

	ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
		args, NULL, filter_graph);
	if (ret < 0) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] cannot create buffer source";
		goto end;
	}

	/* buffer video sink: to terminate the filter chain. */
	ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
		NULL, NULL, filter_graph);
	if (ret < 0) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] cannot create buffer sink";
		goto end;
	}

	ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
		AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] cannot set output pixel format";
		goto end;
	}

	/*
	 * Set the endpoints for the filter graph. The filter_graph will
	 * be linked to the graph described by filters_descr.
	 */

	 /*
	  * The buffer source output must be connected to the input pad of
	  * the first filter described by filters_descr; since the first
	  * filter input label is not specified, it is set to "in" by
	  * default.
	  */
	outputs->name = av_strdup("in");
	outputs->filter_ctx = buffersrc_ctx;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	/*
	 * The buffer sink input must be connected to the output pad of
	 * the last filter described by filters_descr; since the last
	 * filter output label is not specified, it is set to "out" by
	 * default.
	 */
	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = NULL;

	if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
		&inputs, &outputs, NULL)) < 0)
		goto end;

	if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
		goto end;

end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return ret;
}

bool playVideo(string filename)
{
	AVFormatContext* input_ctx = NULL;
	int video_stream, ret;
	AVStream* video = NULL;
	AVCodecContext* decoder_ctx = NULL;
	AVCodec* decoder = NULL;
	AVPacket packet;
	enum AVHWDeviceType type;
	unsigned int in_count;
	unsigned int in_n = 0;
	const char* hwdev = "drm";
	int i;
	drmprime_out_env_t* dpo;
	long loop_count = 2; // just for debugging
	long frame_count = 200;// just for debugging
	const char* out_name = NULL;

	type = av_hwdevice_find_type_by_name(hwdev);
	if (type == AV_HWDEVICE_TYPE_NONE) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] device type " << hwdev << " is not supported.";
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] available device types:";
		while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
			BOOST_LOG_TRIVIAL(error) << "              " << av_hwdevice_get_type_name(type);
		return false;
	}

	dpo = drmprime_out_new();
	if (dpo == NULL) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer]failed to open drmprime output";
		return false;
	}

loopy:

	/* open the input file */
	if (avformat_open_input(&input_ctx, filename.c_str() , NULL, NULL) != 0) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] cannot open input file " << filename;
		return false;
	}

	if (avformat_find_stream_info(input_ctx, NULL) < 0) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] cannot find input stream information.";
		return false;
	}

	/* find the video stream information */
	ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
	if (ret < 0) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] cannot find a video stream in the input file";
		return false;
	}
	video_stream = ret;

	if (decoder->id == AV_CODEC_ID_H264) {
		if ((decoder = avcodec_find_decoder_by_name("h264_v4l2m2m")) == NULL) {
			BOOST_LOG_TRIVIAL(error) << "[videoplayer] cannot find the h264 v4l2m2m decoder";
			return false;
		}
		hw_pix_fmt = AV_PIX_FMT_DRM_PRIME;
	}
	else {
		for (i = 0;; i++) {
			const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
			if (!config) {
				BOOST_LOG_TRIVIAL(error) << "[videoplayer] decoder " << decoder->name << " does not support device type " << av_hwdevice_get_type_name(type);
				return false;
			}
			if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
				config->device_type == type) {
				hw_pix_fmt = config->pix_fmt;
				break;
			}
		}
	}

	if (!(decoder_ctx = avcodec_alloc_context3(decoder))) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] couldn't allocate AV codec";
		return false;
	}

	video = input_ctx->streams[video_stream];
	if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] couldn't get context";
		return false;
	}

	decoder_ctx->get_format = get_hw_format;

	if (hw_decoder_init(decoder_ctx, type) < 0) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] couldn't initialize HW decoder";
		return false;
	}

	decoder_ctx->thread_count = 3;

	if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
		BOOST_LOG_TRIVIAL(error) << "[videoplayer] failed to open codec for stream #" << video_stream;
		return -1;
	}

	/* actual decoding */
	frames = frame_count;
	while (ret >= 0) {
		if ((ret = av_read_frame(input_ctx, &packet)) < 0)
			break;

		if (video_stream == packet.stream_index)
			ret = decode_write(decoder_ctx, dpo, &packet);

		av_packet_unref(&packet);
	}

	/* flush the decoder */
	packet.data = NULL;
	packet.size = 0;
	ret = decode_write(decoder_ctx, dpo, &packet);
	av_packet_unref(&packet);

	avfilter_graph_free(&filter_graph);
	avcodec_free_context(&decoder_ctx);
	avformat_close_input(&input_ctx);

	if (--loop_count > 0)
		goto loopy;

	drmprime_out_delete(dpo);

	return true;
}