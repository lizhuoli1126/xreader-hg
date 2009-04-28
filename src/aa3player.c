/*
 * This file is part of xReader.
 *
 * Copyright (C) 2008 hrimfaxi (outmatch@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <mad.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspaudio.h>
#include <psprtc.h>
#include <pspaudiocodec.h>
#include <limits.h>
#include "config.h"
#include "ssv.h"
#include "strsafe.h"
#include "musicdrv.h"
#include "xmp3audiolib.h"
#include "dbg.h"
#include "scene.h"
#include "apetaglib/APETag.h"
#include "genericplayer.h"
#include "musicinfo.h"
#include "common/utils.h"
#include "xrhal.h"
#include "aa3player.h"
#include "buffered_reader.h"
#include "malloc.h"
#include "mediaengine.h"

#ifdef ENABLE_AA3

#define BUFF_SIZE	8*1152

typedef struct reader_data_t
{
	buffered_reader_t *r;
	int fd;
	bool use_buffer;
	long size;
} aa3_reader_data;

static aa3_reader_data data;

/**
 * MP3音乐播放缓冲
 */
static uint16_t g_buff[BUFF_SIZE / 2];

/**
 * MP3音乐播放缓冲大小，以帧数计
 */
static unsigned g_buff_frame_size;

/**
 * MP3音乐播放缓冲当前位置，以帧数计
 */
static int g_buff_frame_start;


/** 是否使用缓冲IO */
static bool use_buffer = true;

/**
 * 默认缓冲IO缓冲字节大小，最低不小于8192
 */
static int g_io_buffer_size = BUFFERED_READER_BUFFER_SIZE;

/**
 * Media Engine buffer缓存
 */
static unsigned long aa3_codec_buffer[65] __attribute__((aligned(64)));

static short aa3_mix_buffer[2048 * 2] __attribute__((aligned(64)));

#define TYPE_ATRAC3 0x270
#define TYPE_ATRAC3PLUS 0xFFFE

static u16 aa3_type;
static u16 aa3_data_align;
static u32 aa3_data_start;
static u32 aa3_data_size;
static u8 aa3_aa3plus_flagdata[2];
static u16 aa3_channel_mode;
static u32 aa3_sample_per_frame;
static u8* aa3_data_buffer;
static bool aa3_getEDRAM;

/**
 * 初始化驱动变量资源等
 *
 * @return 成功时返回0
 */
static int __init(void)
{
	generic_init();

	generic_lock();
	g_status = ST_UNKNOWN;
	generic_unlock();

	g_seek_seconds = 0;
	g_play_time = 0.;
	memset(&data, 0, sizeof(data));
	memset(&g_info, 0, sizeof(g_info));
	memset(g_buff, 0, sizeof(g_buff));

	data.fd = -1;
	data.use_buffer = use_buffer;

	aa3_type = 0;
	aa3_data_align = 0;
	aa3_data_start = 0;
	aa3_data_size = 0;
	aa3_data_buffer = NULL;
	aa3_getEDRAM = false;

	return 0;
}

/**
 * 停止AA3音乐文件的播放，销毁资源等
 *
 * @note 可以在播放线程中调用
 *
 * @return 成功时返回0
 */
static int __end(void)
{
	xMP3AudioEndPre();

	g_play_time = 0.;
	generic_lock();
	g_status = ST_STOPPED;
	generic_unlock();

	return 0;
}

static int aa3_seek_seconds(double seconds)
{
	int ret;
	u32 frame_index;
	u32 pos;

	if (aa3_sample_per_frame == 0) {
		__end();
		return -1;
	}

	frame_index = (u32)(seconds * g_info.sample_freq / aa3_sample_per_frame);
	pos = aa3_data_start;
	pos += frame_index * aa3_data_align;

	dbg_printf(d, "%s: jump to frame %u pos 0x%08x", __func__, frame_index, pos);

	if (data.use_buffer) {
		ret = buffered_reader_seek(data.r, pos);
	} else {
		ret =
			xrIoLseek(data.fd, pos, SEEK_SET);
	}

	if (ret >= 0) {
		g_buff_frame_size = g_buff_frame_start = 0;
		g_play_time = seconds;
		return 0;
	}

	__end();
	return -1;
}

/**
 * 复制数据到声音缓冲区
 *
 * @note 声音缓存区的格式为双声道，16位低字序
 *
 * @param buf 声音缓冲区指针
 * @param srcbuf 解码数据缓冲区指针
 * @param frames 复制帧数
 * @param channels 声道数
 */
static void send_to_sndbuf(void *buf, uint16_t * srcbuf, int frames,
						   int channels)
{
	int n;
	signed short *p = (signed short *) buf;

	if (frames <= 0)
		return;

	if (channels == 2) {
		memcpy(buf, srcbuf, frames * channels * sizeof(*srcbuf));
	} else {
		for (n = 0; n < frames * channels; n++) {
			*p++ = srcbuf[n];
			*p++ = srcbuf[n];
		}
	}
}

/**
 * MP3音乐播放回调函数，ME版本
 * 负责将解码数据填充声音缓存区
 *
 * @note 声音缓存区的格式为双声道，16位低字序
 *
 * @param buf 声音缓冲区指针
 * @param reqn 缓冲区帧大小
 * @param pdata 用户数据，无用
 */
static int aa3_audiocallback(void *buf, unsigned int reqn, void *pdata)
{
	int avail_frame;
	int snd_buf_frame_size = (int) reqn;
	signed short *audio_buf = buf;
	double incr;

	UNUSED(pdata);

	if (g_status != ST_PLAYING) {
		if (g_status == ST_FFORWARD) {
			g_play_time += g_seek_seconds;
			if (g_play_time >= g_info.duration) {
				__end();
				return -1;
			}
			generic_lock();
			g_status = ST_PLAYING;
			generic_set_playback(true);
			generic_unlock();
			aa3_seek_seconds(g_play_time);
		} else if (g_status == ST_FBACKWARD) {
			g_play_time -= g_seek_seconds;
			if (g_play_time < 0.) {
				g_play_time = 0.;
			}
			generic_lock();
			g_status = ST_PLAYING;
			generic_set_playback(true);
			generic_unlock();
			aa3_seek_seconds(g_play_time);
		}
		xMP3ClearSndBuf(buf, snd_buf_frame_size);
		xrKernelDelayThread(100000);
		return 0;
	}

	while (snd_buf_frame_size > 0) {
		avail_frame = g_buff_frame_size - g_buff_frame_start;

		if (avail_frame >= snd_buf_frame_size) {
			send_to_sndbuf(audio_buf,
						   &g_buff[g_buff_frame_start * 2],
						   snd_buf_frame_size, 2);
			g_buff_frame_start += snd_buf_frame_size;
			audio_buf += snd_buf_frame_size * 2;
			snd_buf_frame_size = 0;
		} else {
			send_to_sndbuf(audio_buf,
						   &g_buff[g_buff_frame_start * 2], avail_frame, 2);
			snd_buf_frame_size -= avail_frame;
			audio_buf += avail_frame * 2;

			int samplesdecoded;
			memset(aa3_mix_buffer, 0, 2048*2*2);
			unsigned long decode_type = 0x1001;
			if ( aa3_type == TYPE_ATRAC3 ) {
				memset( aa3_data_buffer, 0, 0x180);
				if (data.use_buffer) {
					if (buffered_reader_read(data.r, aa3_data_buffer, aa3_data_align) != aa3_data_align) {
						__end();
						return -1;
					}
				} else {
					if (xrIoRead( data.fd, aa3_data_buffer, aa3_data_align ) != aa3_data_align) {
						__end();
						return -1;
					}
				}
				if ( aa3_channel_mode ) {
					memcpy(aa3_data_buffer+aa3_data_align, aa3_data_buffer, aa3_data_align);
				}
				decode_type = 0x1001;
			} else {
				memset( aa3_data_buffer, 0, aa3_data_align+8);
				aa3_data_buffer[0] = 0x0F;
				aa3_data_buffer[1] = 0xD0;
				aa3_data_buffer[2] = aa3_aa3plus_flagdata[0];
				aa3_data_buffer[3] = aa3_aa3plus_flagdata[1];
				if (data.use_buffer) {
					if (buffered_reader_read(data.r, aa3_data_buffer+8, aa3_data_align) != aa3_data_align) {
						__end();
						return -1;
					}
				} else {
					if (xrIoRead( data.fd, aa3_data_buffer+8, aa3_data_align ) != aa3_data_align) {
						__end();
						return -1;
					}
				}
				decode_type = 0x1000;
			}

			aa3_codec_buffer[6] = (unsigned long)aa3_data_buffer;
			aa3_codec_buffer[8] = (unsigned long)aa3_mix_buffer;

			int res = xrAudiocodecDecode(aa3_codec_buffer, decode_type);

			if ( res < 0 ) {
				__end();
				return -1;
			}

			samplesdecoded = aa3_sample_per_frame;

			uint16_t *output = &g_buff[0];

			memcpy(output, aa3_mix_buffer, samplesdecoded * 4);
			g_buff_frame_size = samplesdecoded;
			g_buff_frame_start = 0;
			incr = (double) samplesdecoded / g_info.sample_freq;
			g_play_time += incr;
		}
	}

	return 0;
}

static int aa3_load(const char *spath, const char *lpath)
{
	int ret;

	__init();

	data.fd = xrIoOpen(spath, PSP_O_RDONLY, 0777);

	if (data.fd < 0) {
		goto failed;
	}

   sceIoLseek32(data.fd, 0x0C00, PSP_SEEK_SET);

   u8 ea3_header[0x60];
   if ( sceIoRead( data.fd, ea3_header, 0x60 ) != 0x60 )
      goto wait;
   if ( ea3_header[0] != 0x45 || ea3_header[1] != 0x41 || ea3_header[2] != 0x33 || ea3_header[3] != 0x01 )
      goto wait;

   aa3_type = (ea3_header[0x22] == 0x20) ? TYPE_ATRAC3 : ((ea3_header[0x22] == 0x28) ? TYPE_ATRAC3PLUS : 0x0);

   if ( aa3_type != TYPE_ATRAC3 && aa3_type != TYPE_ATRAC3PLUS )
      goto wait;
   
   g_info.channels = 2;
   g_info.sample_freq = 44100;

   if ( aa3_type == TYPE_ATRAC3 )
      aa3_data_align = ea3_header[0x23]*8;
   else
      aa3_data_align = (ea3_header[0x23]+1)*8;

   aa3_data_start = 0x0C60;
   aa3_data_size = sceIoLseek32(data.fd, 0, PSP_SEEK_END) - aa3_data_start;

   if (aa3_data_size % aa3_data_align != 0 ) {
	   dbg_printf(d, "%s: aa3_data_size %d aa3_data_align %d not align", __func__, aa3_data_size, aa3_data_align);
		goto failed;
   }

   ret = load_me_prx();
   
   if (ret < 0) {
	   dbg_printf(d, "%s: load_me_prx failed", __func__);
	   goto failed;
   }

   if ( aa3_type == TYPE_ATRAC3 ) {
      aa3_channel_mode = 0x0;
	  // atract3 have 3 bitrate, 132k,105k,66k, 132k align=0x180, 105k align = 0x130, 66k align = 0xc0
      
	  if ( aa3_data_align == 0xC0 )
         aa3_channel_mode = 0x1;

      aa3_sample_per_frame = 1024;
      aa3_data_buffer = (u8*)memalign(64, 0x180);

      if ( aa3_data_buffer == NULL)
         goto failed;

      aa3_codec_buffer[26] = 0x20;

      if ( xrAudiocodecCheckNeedMem(aa3_codec_buffer, 0x1001) < 0 )
         goto failed;

      if ( xrAudiocodecGetEDRAM(aa3_codec_buffer, 0x1001) < 0 )
         goto failed;

      aa3_getEDRAM = true;
      aa3_codec_buffer[10] = 4;
      aa3_codec_buffer[44] = 2;

      if ( aa3_data_align == 0x130 )
         aa3_codec_buffer[10] = 6;

      if ( xrAudiocodecInit(aa3_codec_buffer, 0x1001) < 0 ) {
         goto failed;
      }
   } else if ( aa3_type == TYPE_ATRAC3PLUS ) {
      aa3_sample_per_frame = 2048;
      int temp_size = aa3_data_align+8;
      int mod_64 = temp_size & 0x3f;
      if (mod_64 != 0) temp_size += 64 - mod_64;
      aa3_data_buffer = (u8*)memalign(64, temp_size);

      if ( aa3_data_buffer == NULL)
         goto failed;

      aa3_codec_buffer[5] = 0x1;
      aa3_codec_buffer[10] = aa3_aa3plus_flagdata[1];
      aa3_codec_buffer[10] = (aa3_codec_buffer[10] << 8 ) | aa3_aa3plus_flagdata[0];
      aa3_codec_buffer[12] = 0x1;
      aa3_codec_buffer[14] = 0x1;

      if ( xrAudiocodecCheckNeedMem(aa3_codec_buffer, 0x1000) < 0 )
         goto failed;

      if ( xrAudiocodecGetEDRAM(aa3_codec_buffer, 0x1000) < 0 )
         goto failed;

      aa3_getEDRAM = true;

      if ( xrAudiocodecInit(aa3_codec_buffer, 0x1000) < 0 ) {
         goto failed;
      }
   } else {
      goto failed;
   }

   if (g_info.sample_freq != 0 && aa3_data_align != 0) {
	   g_info.duration = (double)aa3_data_size * aa3_sample_per_frame / aa3_data_align / g_info.sample_freq;
   } else {
	   g_info.duration = 0;
   }

   g_info.avg_bps = (double) aa3_data_size * 8 / g_info.duration;

   if (data.use_buffer) {
	   SceOff cur = xrIoLseek(data.fd, 0, PSP_SEEK_CUR);

	   xrIoClose(data.fd);
	   data.fd = -1;
	   data.r = buffered_reader_open(spath, g_io_buffer_size, 1);
	   buffered_reader_seek(data.r, cur);
   }

   ret = xMP3AudioInit();

   if (ret < 0) {
		goto failed;
   }

   ret = xMP3AudioSetFrequency(g_info.sample_freq);

   if (ret < 0) {
		goto failed;
   }

   xMP3AudioSetChannelCallback(0, aa3_audiocallback, NULL);

   return 0;
   
failed:
   __end();
   return -1;
}

static int aa3_end(void)
{
	dbg_printf(d, "%s", __func__);

	__end();

	xMP3AudioEnd();

	g_status = ST_STOPPED;
	generic_end();

	if ( data.use_buffer && data.r != NULL) {
		buffered_reader_close(data.r);
		data.r = NULL;
	}

	if ( data.fd >= 0) {
		xrIoClose(data.fd);
		data.fd = -1;
	}

	if ( aa3_data_buffer) {
		free(aa3_data_buffer);
		aa3_data_buffer = NULL;
	}

	if (aa3_getEDRAM) {
		xrAudiocodecReleaseEDRAM(aa3_codec_buffer);
		aa3_getEDRAM = false;
	}

	return 0;
}

static int aa3_get_info(struct music_info *info)
{
	if (g_status == ST_UNKNOWN) {
		return -1;
	}

	if (info->type & MD_GET_CURTIME) {
		info->cur_time = g_play_time;
	}
	if (info->type & MD_GET_CPUFREQ) {
		if (data.use_buffer)
			info->psp_freq[0] = 49;
		else
			info->psp_freq[0] = 33;

		info->psp_freq[1] = 16;
	}
	if (info->type & MD_GET_DECODERNAME) {
		if (aa3_type == TYPE_ATRAC3)
			STRCPY_S(info->decoder_name, "atrac3");
		else if (aa3_type == TYPE_ATRAC3PLUS)
			STRCPY_S(info->decoder_name, "atrac3plus");
		else
			STRCPY_S(info->decoder_name, "aa3");
	}
	if (info->type & MD_GET_ENCODEMSG) {
		info->encode_msg[0] = '\0';
	}

	return generic_get_info(info);
}

/**
 * 检测是否为AA3文件，目前只检查文件后缀名
 *
 * @param spath 当前播放音乐名，8.3路径形式
 *
 * @return 是AA3文件返回1，否则返回0
 */
static int aa3_probe(const char *spath)
{
	const char *p;

	p = utils_fileext(spath);

	if (p) {
		if (stricmp(p, "aa3") == 0) {
			return 1;
		}
		if (stricmp(p, "oma") == 0) {
			return 1;
		}
	}

	return 0;
}

/**
 * PSP准备休眠时aa3e的操作
 *
 * @return 成功时返回0
 */
static int aa3_suspend(void)
{
	generic_suspend();
	aa3_end();

	return 0;
}

/**
 * PSP准备从休眠时恢复的aa3e的操作
 *
 * @param spath 当前播放音乐名，8.3路径形式
 * @param lpath 当前播放音乐名，长文件或形式
 *
 * @return 成功时返回0
 */
static int aa3_resume(const char *spath, const char *lpath)
{
	int ret;

	ret = aa3_load(spath, lpath);

	if (ret != 0) {
		dbg_printf(d, "%s: aa3_load failed %d", __func__, ret);
		return -1;
	}

	g_play_time = g_suspend_playing_time;
	aa3_seek_seconds(g_play_time);
	g_suspend_playing_time = 0;

	generic_resume(spath, lpath);

	return 0;
}

static struct music_ops aa3_ops = {
	.name = "aa3",
	.set_opt = NULL,
	.load = aa3_load,
	.play = NULL,
	.pause = NULL,
	.end = aa3_end,
	.get_status = NULL,
	.fforward = NULL,
	.fbackward = NULL,
	.suspend = aa3_suspend,
	.resume = aa3_resume,
	.get_info = aa3_get_info,
	.probe = aa3_probe,
	.next = NULL,
};

int aa3_init()
{
	return register_musicdrv(&aa3_ops);
}

#endif
