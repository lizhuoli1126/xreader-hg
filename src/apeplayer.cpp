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

#include <pspkernel.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define ENABLE_MUSIC
#include "scene.h"
#include "xmp3audiolib.h"
#include "musicmgr.h"
#include "musicdrv.h"
#include "All.h"
#include "CharacterHelper.h"
#include "APEDecompress.h"
#include "APEInfo.h"
#include "strsafe.h"
#include "common/utils.h"
#include "apetaglib/APETag.h"
#include "dbg.h"
#include "ssv.h"

#define BLOCKS_PER_DECODE (4096 / 4)
#define NUM_AUDIO_SAMPLES (BLOCKS_PER_DECODE * 4)

static int __end(void);

/**
 * 当前驱动播放状态
 */
static int g_status;

/**
 * 休眠前播放状态
 */
static int g_suspend_status;

/**
 * APE音乐播放缓冲
 */
static short *g_buff = NULL;

/**
 * APE音乐播放缓冲大小，以帧数计
 */
static unsigned g_buff_frame_size;

/**
 * APE音乐播放缓冲当前位置，以帧数计
 */
static int g_buff_frame_start;

/**
 * 当前驱动播放状态写锁
 */
static SceUID g_status_sema = -1;

/**
 * APE音乐文件长度，以秒数
 */
static double g_duration;

/**
 * 当前播放时间，以秒数计
 */
static double g_play_time;

/**
 * APE音乐快进、退秒数
 */
static int g_seek_seconds;

/**
 * APE音乐声道数
 */
static int g_ape_channels;

/**
 * APE音乐声道数
 */
static int g_ape_sample_freq;

/**
 * APE音乐比特率
 */
static float g_ape_bitrate;

/**
 * APE总帧数
 */
static int g_ape_total_samples = 0;

/**
 * APE音乐每样本位数
 */
static int g_ape_bits_per_sample = 0;

/**
 * APE文件大小
 */
static uint32_t g_ape_file_size = 0;

/**
 * APE音乐休眠时播放时间
 */
static double g_suspend_playing_time;

typedef struct _ape_taginfo_t
{
	char title[80];
	char artist[80];
	char album[80];
} ape_taginfo_t;

static ape_taginfo_t g_taginfo;

/**
 * APE编码器名字
 */
static char g_encode_name[80];

/**
 * APE解码器
 */
static CAPEDecompress *g_decoder = NULL;

/**
 * 加锁
 */
static inline int ape_lock(void)
{
	return sceKernelWaitSemaCB(g_status_sema, 1, NULL);
}

/**
 * 解锁
 */
static inline int ape_unlock(void)
{
	return sceKernelSignalSema(g_status_sema, 1);
}

/**
 * 设置APE音乐播放选项
 *
 * @param key
 * @param value
 *
 * @return 成功时返回0
 */
static int ape_set_opt(const char *key, const char *values)
{
	int argc, i;
	char **argv;

	dbg_printf(d, "%s: options are %s", __func__, values);

	build_args(values, &argc, &argv);

	for (i = 0; i < argc; ++i) {
		if (!strncasecmp
			(argv[i], "show_encoder_msg", sizeof("show_encoder_msg") - 1)) {
			if (opt_is_on(argv[i])) {
				show_encoder_msg = true;
			} else {
				show_encoder_msg = false;
			}
		}
	}

	clean_args(argc, argv);

	return 0;
}

/**
 * 清空声音缓冲区
 *
 * @param buf 声音缓冲区指针
 * @param frames 帧数大小
 */
static void clear_snd_buf(void *buf, int frames)
{
	memset(buf, 0, frames * 2 * 2);
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
static void send_to_sndbuf(void *buf, short *srcbuf, int frames, int channels)
{
	int n;
	signed short *p = (signed short *) buf;

	if (frames <= 0)
		return;

	for (n = 0; n < frames * channels; n++) {
		if (channels == 2)
			*p++ = srcbuf[n];
		else if (channels == 1) {
			*p++ = srcbuf[n];
			*p++ = srcbuf[n];
		}
	}
}

static int ape_seek_seconds(double seconds)
{
	uint32_t sample;

	if (g_duration == 0)
		return -1;

	if (seconds >= g_duration) {
		__end();
		return 0;
	}

	if (seconds < 0)
		seconds = 0;

	free_bitrate(&g_inst_br);
	sample = g_ape_total_samples * seconds / g_duration;

	dbg_printf(d, "Seeking to sample %d.", sample);

	int ret = g_decoder->Seek(sample);

	if (ret == ERROR_SUCCESS) {
		g_play_time = seconds;

		return 0;
	}

	return -1;
}

/**
 * APE音乐播放回调函数，
 * 负责将解码数据填充声音缓存区
 *
 * @note 声音缓存区的格式为双声道，16位低字序
 *
 * @param buf 声音缓冲区指针
 * @param reqn 缓冲区帧大小
 * @param pdata 用户数据，无用
 */
static int ape_audiocallback(void *buf, unsigned int reqn, void *pdata)
{
	int avail_frame;
	int snd_buf_frame_size = (int) reqn;
	int ret;
	double incr;
	signed short *audio_buf = (signed short *) buf;

	UNUSED(pdata);

	if (g_status != ST_PLAYING) {
		if (g_status == ST_FFOWARD) {
			ape_lock();
			g_status = ST_PLAYING;
			scene_power_save(true);
			ape_unlock();
			ape_seek_seconds(g_play_time + g_seek_seconds);
		} else if (g_status == ST_FBACKWARD) {
			ape_lock();
			g_status = ST_PLAYING;
			scene_power_save(true);
			ape_unlock();
			ape_seek_seconds(g_play_time - g_seek_seconds);
		}
		clear_snd_buf(buf, snd_buf_frame_size);
		return 0;
	}

	while (snd_buf_frame_size > 0) {
		avail_frame = g_buff_frame_size - g_buff_frame_start;

		if (avail_frame >= snd_buf_frame_size) {
			send_to_sndbuf(audio_buf,
						   &g_buff[g_buff_frame_start * g_ape_channels],
						   snd_buf_frame_size, g_ape_channels);
			g_buff_frame_start += snd_buf_frame_size;
			audio_buf += snd_buf_frame_size * 2;
			snd_buf_frame_size = 0;
		} else {
			send_to_sndbuf(audio_buf,
						   &g_buff[g_buff_frame_start * g_ape_channels],
						   avail_frame, g_ape_channels);
			snd_buf_frame_size -= avail_frame;
			audio_buf += avail_frame * 2;
			int block = -1;

			ret =
				g_decoder->GetData((char *) g_buff, BLOCKS_PER_DECODE, &block);

			if (ret != ERROR_SUCCESS) {
				__end();
				return -1;
			}

			g_buff_frame_size = block;
			g_buff_frame_start = 0;

			incr = 1.0 * g_buff_frame_size / g_ape_sample_freq;
			g_play_time += incr;

			add_bitrate(&g_inst_br,
						g_decoder->GetInfo(APE_DECOMPRESS_CURRENT_BITRATE) *
						1000, incr);
		}
	}

	return 0;
}

/**
 * 初始化驱动变量资源等
 *
 * @return 成功时返回0
 */
static int __init(void)
{
	g_status_sema = sceKernelCreateSema("wave Sema", 0, 1, 1, NULL);

	ape_lock();
	g_status = ST_UNKNOWN;
	ape_unlock();

	g_buff_frame_size = g_buff_frame_start = 0;
	g_seek_seconds = 0;
	g_duration = g_play_time = 0.;
	g_ape_bitrate = g_ape_sample_freq = g_ape_channels = 0;;
	memset(&g_taginfo, 0, sizeof(g_taginfo));
	g_decoder = NULL;
	g_encode_name[0] = '\0';

	return 0;
}

/**
 * 装载APE音乐文件 
 *
 * @param spath 短路径名
 * @param lpath 长路径名
 *
 * @return 成功时返回0
 */
static int ape_load(const char *spath, const char *lpath)
{
	__init();

	APETag *tag = loadAPETag(spath);

	if (tag != NULL) {
		char *title = APETag_SimpleGet(tag, "Title");
		char *artist = APETag_SimpleGet(tag, "Artist");
		char *album = APETag_SimpleGet(tag, "Album");

		if (title) {
			STRCPY_S(g_taginfo.title, title);
			free(title);
			title = NULL;
		}
		if (artist) {
			STRCPY_S(g_taginfo.artist, artist);
			free(artist);
			artist = NULL;
		} else {
			artist = APETag_SimpleGet(tag, "Album artist");
			if (artist) {
				STRCPY_S(g_taginfo.artist, artist);
				free(artist);
				artist = NULL;
			}
		}
		if (album) {
			STRCPY_S(g_taginfo.album, album);
			free(album);
			album = NULL;
		}
		freeAPETag(tag);
	}

	if (g_buff != NULL) {
		free(g_buff);
		g_buff = NULL;
	}

	g_buff = (short int *) calloc(NUM_AUDIO_SAMPLES, sizeof(*g_buff));

	if (g_buff == NULL) {
		__end();
		return -1;
	}

	CSmartPtr <str_utf16> path(GetUTF16FromANSI(spath));
	int err;
	CAPEInfo ape_info(&err, path);

	if (err == ERROR_SUCCESS) {
		g_ape_sample_freq = ape_info.GetInfo(APE_INFO_SAMPLE_RATE);
		g_ape_channels = ape_info.GetInfo(APE_INFO_CHANNELS);
		g_ape_total_samples = ape_info.GetInfo(APE_INFO_TOTAL_BLOCKS);
		g_ape_file_size = ape_info.GetInfo(APE_INFO_APE_TOTAL_BYTES);
		g_ape_bitrate = ape_info.GetInfo(APE_INFO_AVERAGE_BITRATE) * 1000;
		g_ape_bits_per_sample = ape_info.GetInfo(APE_INFO_BITS_PER_SAMPLE);

		if (g_ape_total_samples > 0) {
			g_duration = 1.0 * g_ape_total_samples / g_ape_sample_freq;
		} else {
			g_duration = 0;
		}
	} else {
		__end();
		return -1;
	}

	if (xMP3AudioInit() < 0) {
		__end();
		return -1;
	}

	if (xMP3AudioSetFrequency(g_ape_sample_freq) < 0) {
		__end();
		return -1;
	}

	ape_lock();
	g_status = ST_LOADED;
	ape_unlock();

	dbg_printf(d,
			   "[%d channel(s), %d Hz, %.2f kbps, %02d:%02d, encoder: %s, Ratio: %.3f]",
			   g_ape_channels, g_ape_sample_freq, g_ape_bitrate / 1000,
			   (int) (g_duration / 60), (int) g_duration % 60, g_encode_name,
			   1.0 * g_ape_file_size / (g_ape_total_samples *
										g_ape_channels *
										(g_ape_bits_per_sample / 8))
		);

	dbg_printf(d, "[%s - %s - %s, ape tag]", g_taginfo.artist, g_taginfo.album,
			   g_taginfo.title);

	g_decoder = (CAPEDecompress *) CreateIAPEDecompress(path, &err);

	if (err != ERROR_SUCCESS) {
		__end();
		return -1;
	}

	xMP3AudioSetChannelCallback(0, ape_audiocallback, NULL);

	return 0;
}

/**
 * 开始APE音乐文件的播放
 *
 * @return 成功时返回0
 */
static int ape_play(void)
{
	ape_lock();
	scene_power_playing_music(true);
	g_status = ST_PLAYING;
	ape_unlock();

	return 0;
}

/**
 * 暂停APE音乐文件的播放
 *
 * @return 成功时返回0
 */
static int ape_pause(void)
{
	ape_lock();
	scene_power_playing_music(false);
	g_status = ST_PAUSED;
	ape_unlock();

	return 0;
}

/**
 * 停止APE音乐文件的播放，销毁资源等
 *
 * @note 可以在播放线程中调用
 *
 * @return 成功时返回0
 */
static int __end(void)
{
	xMP3AudioEndPre();

	ape_lock();
	g_status = ST_STOPPED;
	ape_unlock();

	if (g_status_sema >= 0) {
		sceKernelDeleteSema(g_status_sema);
		g_status_sema = -1;
	}

	g_play_time = 0.;

	return 0;
}

/**
 * 停止APE音乐文件的播放，销毁所占有的线程、资源等
 *
 * @note 不可以在播放线程中调用，必须能够多次重复调用而不死机
 *
 * @return 成功时返回0
 */
static int ape_end(void)
{
	__end();

	xMP3AudioEnd();

	if (g_buff != NULL) {
		free(g_buff);
		g_buff = NULL;
	}

	g_status = ST_STOPPED;

	if (g_decoder) {
		delete g_decoder;

		g_decoder = NULL;
	}

	free_bitrate(&g_inst_br);

	return 0;
}

/**
 * 得到当前驱动的播放状态
 *
 * @return 状态
 */
static int ape_get_status(void)
{
	return g_status;
}

/**
 * 快进APE音乐文件
 *
 * @param sec 秒数
 *
 * @return 成功时返回0
 */
static int ape_fforward(int sec)
{
	ape_lock();
	if (g_status == ST_PLAYING || g_status == ST_PAUSED)
		g_status = ST_FFOWARD;
	ape_unlock();

	g_seek_seconds = sec;

	return 0;
}

/**
 * 快退APE音乐文件
 *
 * @param sec 秒数
 *
 * @return 成功时返回0
 */
static int ape_fbackward(int sec)
{
	ape_lock();
	if (g_status == ST_PLAYING || g_status == ST_PAUSED)
		g_status = ST_FBACKWARD;
	ape_unlock();

	g_seek_seconds = sec;

	return 0;
}

/**
 * PSP准备休眠时APE的操作
 *
 * @return 成功时返回0
 */
static int ape_suspend(void)
{
	g_suspend_status = g_status;
	g_suspend_playing_time = g_play_time;
	ape_end();

	return 0;
}

/**
 * PSP准备从休眠时恢复的APE的操作
 *
 * @param spath 当前播放音乐名，8.3路径形式
 * @param lpath 当前播放音乐名，长文件或形式
 *
 * @return 成功时返回0
 */
static int ape_resume(const char *spath, const char *lpath)
{
	int ret;

	ret = ape_load(spath, lpath);
	if (ret != 0) {
		dbg_printf(d, "%s: ape_load failed %d", __func__, ret);
		return -1;
	}

	g_play_time = g_suspend_playing_time;
	ape_seek_seconds(g_play_time);
	g_suspend_playing_time = 0;

	ape_lock();
	g_status = g_suspend_status;
	ape_unlock();
	g_suspend_status = ST_LOADED;

	return 0;
}

/**
 * 得到APE音乐文件相关信息
 *
 * @param pinfo 信息结构体指针
 *
 * @return
 */
static int ape_get_info(struct music_info *pinfo)
{
	if (pinfo->type & MD_GET_TITLE) {
		pinfo->encode = conf_encode_utf8;
		STRCPY_S(pinfo->title, g_taginfo.title);
	}
	if (pinfo->type & MD_GET_ALBUM) {
		pinfo->encode = conf_encode_utf8;
		STRCPY_S(pinfo->album, g_taginfo.album);
	}
	if (pinfo->type & MD_GET_ARTIST) {
		pinfo->encode = conf_encode_utf8;
		STRCPY_S(pinfo->artist, g_taginfo.artist);
	}
	if (pinfo->type & MD_GET_COMMENT) {
		pinfo->encode = conf_encode_utf8;
		STRCPY_S(pinfo->comment, "");
	}
	if (pinfo->type & MD_GET_CURTIME) {
		pinfo->cur_time = g_play_time;
	}
	if (pinfo->type & MD_GET_DURATION) {
		pinfo->duration = g_duration;
	}
	if (pinfo->type & MD_GET_CPUFREQ) {
		pinfo->psp_freq[0] = 222;
		pinfo->psp_freq[1] = 111;
	}
	if (pinfo->type & MD_GET_FREQ) {
		pinfo->freq = g_ape_sample_freq;
	}
	if (pinfo->type & MD_GET_CHANNELS) {
		pinfo->channels = g_ape_channels;
	}
	if (pinfo->type & MD_GET_AVGKBPS) {
		pinfo->avg_kbps = g_ape_bitrate / 1000;
	}
	if (pinfo->type & MD_GET_INSKBPS) {
		pinfo->ins_kbps = get_inst_bitrate(&g_inst_br) / 1000;
	}
	if (pinfo->type & MD_GET_DECODERNAME) {
		STRCPY_S(pinfo->decoder_name, "ape");
	}
	if (pinfo->type & MD_GET_ENCODEMSG) {
		if (show_encoder_msg) {
			SPRINTF_S(pinfo->encode_msg, "%s Ratio: %.3f", g_encode_name,
					  1.0 * g_ape_file_size / (g_ape_total_samples *
											   g_ape_channels *
											   (g_ape_bits_per_sample / 8)));
		} else {
			pinfo->encode_msg[0] = '\0';
		}
	}

	return 0;
}

static struct music_ops ape_ops = {
	"ape",
	ape_set_opt,
	ape_load,
	ape_play,
	ape_pause,
	ape_fforward,
	ape_fbackward,
	ape_get_status,
	ape_get_info,
	ape_suspend,
	ape_resume,
	ape_end,
	NULL
};

extern "C" int ape_init(void)
{
	return register_musicdrv(&ape_ops);
}
