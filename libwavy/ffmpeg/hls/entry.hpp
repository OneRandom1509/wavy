#pragma once
/************************************************
 * Wavy Project - High-Fidelity Audio Streaming
 * ---------------------------------------------
 * 
 * Copyright (c) 2025 Oinkognito
 * All rights reserved.
 * 
 * This source code is part of the Wavy project, an advanced
 * local networking solution for high-quality audio streaming.
 * 
 * License:
 * This software is licensed under the BSD-3-Clause License.
 * You may use, modify, and distribute this software under
 * the conditions stated in the LICENSE file provided in the
 * project root.
 * 
 * Warranty Disclaimer:
 * This software is provided "AS IS," without any warranties
 * or guarantees, either expressed or implied, including but
 * not limited to fitness for a particular purpose.
 * 
 * Contributions:
 * Contributions to this project are welcome. By submitting 
 * code, you agree to license your contributions under the 
 * same BSD-3-Clause terms.
 * 
 * See LICENSE file for full details.
 ************************************************/

#include <filesystem>
#include <fstream>
#include <libwavy/common/macros.hpp>
#include <libwavy/ffmpeg/misc/metadata.hpp>
#include <libwavy/logger.hpp>
#include <regex>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace fs = std::filesystem;

namespace libwavy::hls
{

class HLS_Segmenter
{
public:
  libwavy::ffmpeg::Metadata lbwMetadata;
  HLS_Segmenter() { avformat_network_init(); }

  ~HLS_Segmenter() { avformat_network_deinit(); }

  void create_hls_segments(const char* input_file, const char* output_dir, bool use_flac = false)
  {
    std::vector<std::string> playlist_files;

    int bitrate = lbwMetadata.fetchBitrate(input_file);

    LOG_DEBUG << "Found bitrate: " << bitrate;

    std::string codec_prefix    = use_flac ? "flac" : "mp3";
    std::string output_playlist = std::string(output_dir) + "/hls_" + codec_prefix + "_" +
                                  std::to_string(bitrate) + macros::to_string(macros::PLAYLIST_EXT);
    playlist_files.push_back(output_playlist);

    bool success = use_flac ? encode_flac_variant(input_file, output_playlist.c_str())
                            : encode_variant(input_file, output_playlist.c_str(), bitrate);

    if (!success)
    {
      av_log(nullptr, AV_LOG_ERROR, "Encoding failed for file %s\n", input_file);
      return;
    }
  }

  void create_master_playlist(const std::string& input_dir, const std::string& output_dir,
                              bool use_flac)
  {
    std::vector<std::string> playlists;
    std::vector<int>         bitrates;

    std::regex bitrate_regex(R"(hls_mp3_(\d+)\.m3u8$)"); // Matches "hls_mp3_<bitrate>.m3u8"

    for (const auto& entry : fs::directory_iterator(input_dir))
    {
      if (entry.is_regular_file() && entry.path().extension() == ".m3u8")
      {
        std::string filename = entry.path().filename().string();
        std::smatch match;
        if (std::regex_search(filename, match, bitrate_regex))
        {
          playlists.push_back(filename);
          bitrates.push_back(std::stoi(match[1])); // Extract bitrate
        }
      }
    }

    if (playlists.empty())
    {
      std::cerr << "No playlists found in directory: " << input_dir << "\n";
      return;
    }

    std::string   master_playlist = output_dir + "/" + macros::to_string(macros::MASTER_PLAYLIST);
    std::ofstream m3u8(master_playlist);

    if (!m3u8)
    {
      av_log(nullptr, AV_LOG_ERROR, "Failed to create master playlist\n");
      return;
    }

    m3u8 << macros::MASTER_PLAYLIST_HEADER;

    for (size_t i = 0; i < playlists.size(); i++)
    {
      m3u8 << "#EXT-X-STREAM-INF:BANDWIDTH=" << bitrates[i] << ",CODECS=\""
           << (use_flac ? "fLaC" : "mp4a.40.2") << "\"\n";
      m3u8 << playlists[i] << "\n";
    }

    m3u8.close();
    if (use_flac)
      LOG_INFO << "Created HLS segments for FLAC with references written to: "
               << macros::to_string(macros::MASTER_PLAYLIST);
    else
      LOG_INFO << "Created HLS segments for LOSSY with references written to master playlist: "
               << macros::to_string(macros::MASTER_PLAYLIST);
  }

private:
  auto encode_variant(const char* input_file, const char* output_playlist, int bitrate) -> bool
  {
    AVFormatContext* input_ctx          = nullptr;
    AVFormatContext* output_ctx         = nullptr;
    AVStream*        audio_stream       = nullptr;
    int              audio_stream_index = -1;
    bool             is_flac            = false;
    AVDictionary*    options            = nullptr;

    if (avformat_open_input(&input_ctx, input_file, nullptr, nullptr) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Failed to open input file: %s\n", input_file);
      return false;
    }

    if (avformat_find_stream_info(input_ctx, nullptr) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Failed to retrieve stream info\n");
      avformat_close_input(&input_ctx);
      return false;
    }

    // Find audio stream
    for (unsigned int i = 0; i < input_ctx->nb_streams; i++)
    {
      if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      {
        audio_stream_index = i;
        break;
      }
    }

    if (audio_stream_index == -1)
    {
      av_log(nullptr, AV_LOG_ERROR, "No audio stream found\n");
      avformat_close_input(&input_ctx);
      return false;
    }

    // Allocate output context for HLS (do not change the muxer later)
    if (avformat_alloc_output_context2(&output_ctx, nullptr, "hls", output_playlist) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Failed to allocate output context\n");
      avformat_close_input(&input_ctx);
      return false;
    }

    // Create new stream in output context
    audio_stream = avformat_new_stream(output_ctx, nullptr);
    if (!audio_stream)
    {
      av_log(nullptr, AV_LOG_ERROR, "Failed to create new stream\n");
      avformat_close_input(&input_ctx);
      avformat_free_context(output_ctx);
      return false;
    }

    if (avcodec_parameters_copy(audio_stream->codecpar,
                                input_ctx->streams[audio_stream_index]->codecpar) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Failed to copy codec parameters\n");
      avformat_close_input(&input_ctx);
      avformat_free_context(output_ctx);
      return false;
    }

    audio_stream->codecpar->bit_rate = bitrate * 1000; // kbps to bps conversion

    // Prepare segment filename
    std::string output_playlist_str = output_playlist;
    size_t      last_slash          = output_playlist_str.find_last_of('/');
    std::string out_dir =
      (last_slash != std::string::npos) ? output_playlist_str.substr(0, last_slash) : ".";
    std::string segment_filename_format =
      out_dir + "/hls_mp3_" + std::to_string(bitrate) + "_%d.ts";

    // Set HLS options common to both cases
    av_dict_set(&options, macros::to_string(macros::CODEC_HLS_TIME_FIELD).c_str(), "10", 0);
    av_dict_set(&options, macros::to_string(macros::CODEC_HLS_LIST_SIZE_FIELD).c_str(), "0", 0);
    av_dict_set(&options, macros::to_string(macros::CODEC_HLS_FLAGS_FIELD).c_str(),
                "independent_segments", 0);
    av_dict_set(&options, macros::to_string(macros::CODEC_HLS_SEGMENT_FILENAME_FIELD).c_str(),
                segment_filename_format.c_str(), 0);

    // Write header (after all options and codec parameter changes)
    if (avformat_write_header(output_ctx, &options) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Error occurred while writing header\n");
      av_dict_free(&options);
      avformat_close_input(&input_ctx);
      avformat_free_context(output_ctx);
      return false;
    }

    // Packet processing loop
    AVPacket pkt;
    while (av_read_frame(input_ctx, &pkt) >= 0)
    {
      if (pkt.stream_index == audio_stream_index)
      {
        pkt.stream_index = audio_stream->index;
        pkt.pts          = av_rescale_q(pkt.pts, input_ctx->streams[audio_stream_index]->time_base,
                                        audio_stream->time_base);
        pkt.dts          = av_rescale_q(pkt.dts, input_ctx->streams[audio_stream_index]->time_base,
                                        audio_stream->time_base);
        pkt.duration = av_rescale_q(pkt.duration, input_ctx->streams[audio_stream_index]->time_base,
                                    audio_stream->time_base);

        if (av_interleaved_write_frame(output_ctx, &pkt) < 0)
        {
          av_log(nullptr, AV_LOG_ERROR, "Error writing frame\n");
          av_packet_unref(&pkt);
          break;
        }
      }
      av_packet_unref(&pkt);
    }

    av_write_trailer(output_ctx);

    av_dict_free(&options);
    avformat_close_input(&input_ctx);
    if (output_ctx)
    {
      if (!(output_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&output_ctx->pb);
      avformat_free_context(output_ctx);
    }

    return true;
  }

  auto encode_flac_variant(const char* input_file, const char* output_playlist) -> bool
  {
    AVFormatContext *input_ctx = nullptr, *output_ctx = nullptr;
    AVStream *       in_stream = nullptr, *out_stream = nullptr;
    AVPacket*        pkt = nullptr;
    int              ret, audio_stream_idx = -1;
    std::string      output_playlist_str = output_playlist;
    size_t           last_slash          = output_playlist_str.find_last_of('/');
    std::string      out_dir =
      (last_slash != std::string::npos) ? output_playlist_str.substr(0, last_slash) : ".";
    std::string segment_filename_format = out_dir + "/hls_flac" + "_%d.m4s";

    // Open input file
    if ((ret = avformat_open_input(&input_ctx, input_file, nullptr, nullptr)) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Error opening input file.\n");
      return ret;
    }

    if ((ret = avformat_find_stream_info(input_ctx, nullptr)) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Error finding stream info.\n");
      goto cleanup;
    }

    // Find audio stream
    for (int i = 0; i < input_ctx->nb_streams; i++)
    {
      if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      {
        audio_stream_idx = i;
        break;
      }
    }

    if (audio_stream_idx == -1)
    {
      av_log(nullptr, AV_LOG_ERROR, "No audio stream found.\n");
      ret = AVERROR(EINVAL);
      goto cleanup;
    }

    // Create output context
    avformat_alloc_output_context2(&output_ctx, nullptr, "hls", output_playlist);
    if (!output_ctx)
    {
      fprintf(stderr, "Error creating output context\n");
      ret = AVERROR_UNKNOWN;
      goto cleanup;
    }

    // Set HLS muxer options
    av_opt_set(output_ctx->priv_data, "hls_segment_type", "fmp4", 0);
    av_opt_set(output_ctx->priv_data, "hls_playlist_type", "vod", 0);
    av_opt_set(output_ctx->priv_data,
               macros::to_string(macros::CODEC_HLS_SEGMENT_FILENAME_FIELD).c_str(),
               segment_filename_format.c_str(), 0);
    av_opt_set(output_ctx->priv_data, "master_pl_name",
               macros::to_string(macros::MASTER_PLAYLIST).c_str(), 0);

    // Create output stream
    out_stream = avformat_new_stream(output_ctx, nullptr);
    if (!out_stream)
    {
      fprintf(stderr, "Error creating output stream\n");
      ret = AVERROR_UNKNOWN;
      goto cleanup;
    }

    // Copy codec parameters
    in_stream = input_ctx->streams[audio_stream_idx];
    if ((ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar)) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Error copying codec parameters.\n");
      goto cleanup;
    }

    // Open output file
    if (!(output_ctx->oformat->flags & AVFMT_NOFILE))
    {
      if ((ret = avio_open(&output_ctx->pb, output_ctx->url, AVIO_FLAG_WRITE)) < 0)
      {
        av_log(nullptr, AV_LOG_ERROR, "Error opening output file.\n");
        goto cleanup;
      }
    }

    // Write header
    if ((ret = avformat_write_header(output_ctx, nullptr)) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Error writing header.\n");
      goto cleanup;
    }

    // Allocate packet
    pkt = av_packet_alloc();
    if (!pkt)
    {
      av_log(nullptr, AV_LOG_ERROR, "Error allocating packet\n");
      ret = AVERROR(ENOMEM);
      goto cleanup;
    }

    while (av_read_frame(input_ctx, pkt) >= 0)
    {
      if (pkt->stream_index == audio_stream_idx)
      {
        // Rescale timestamps
        pkt->pts      = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base,
                                         AV_ROUND_NEAR_INF);
        pkt->dts      = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base,
                                         AV_ROUND_NEAR_INF);
        pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
        pkt->pos      = -1;
        pkt->stream_index = 0;

        if ((ret = av_interleaved_write_frame(output_ctx, pkt)) < 0)
        {
          av_log(nullptr, AV_LOG_ERROR, "Error writing packet.\n");
          break;
        }
      }
      av_packet_unref(pkt);
    }

    // Write trailer
    av_write_trailer(output_ctx);

  cleanup:
    if (pkt)
      av_packet_free(&pkt);
    if (output_ctx && !(output_ctx->oformat->flags & AVFMT_NOFILE))
      avio_closep(&output_ctx->pb);
    if (output_ctx)
      avformat_free_context(output_ctx);
    if (input_ctx)
      avformat_close_input(&input_ctx);
    return ret < 0 ? false : true;
  }
};

} // namespace libwavy::hls
