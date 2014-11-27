/* Arcan-fe, scriptable front-end engine
 *
 * Arcan-fe is the legal property of its developers, please refer
 * to the COPYRIGHT file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <assert.h>
#include <limits.h>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#endif

#include "arcan_math.h"
#include "arcan_general.h"
#include "arcan_shmif.h"
#include "arcan_event.h"
#include "arcan_video.h"
#include "arcan_videoint.h"
#include "arcan_audio.h"
#include "arcan_audioint.h"
#include "arcan_frameserver.h"
#include "arcan_event.h"

static uint64_t cookie;

static inline void emit_deliveredframe(arcan_frameserver* src,
	unsigned long long pts, unsigned long long framecount);
static inline void emit_droppedframe(arcan_frameserver* src,
	unsigned long long pts, unsigned long long framecount);

arcan_errc arcan_frameserver_free(arcan_frameserver* src)
{
	if (!src)
		return ARCAN_ERRC_NO_SUCH_OBJECT;

	struct arcan_shmif_page* shmpage = (struct arcan_shmif_page*)
		src->shm.ptr;

	if (!src->flags.alive)
		return ARCAN_ERRC_UNACCEPTED_STATE;

	if (shmpage){
		if (arcan_frameserver_enter(src)){
			arcan_event exev = {
				.category = EVENT_TARGET,
			.kind = TARGET_COMMAND_EXIT
			};
			arcan_frameserver_pushevent(src, &exev);

			shmpage->dms = false;
			shmpage->vready = false;
			shmpage->aready = false;
			arcan_sem_post( src->vsync );
			arcan_sem_post( src->async );
		}

		arcan_frameserver_dropshared(src);
		src->shm.ptr = NULL;

		arcan_frameserver_leave();
	}

	arcan_frameserver_killchild(src);

	src->child = BROKEN_PROCESS_HANDLE;
	src->flags.alive = false;

/* unhook audio monitors */
	arcan_aobj_id* base = src->alocks;
	while (base && *base){
		arcan_audio_hookfeed(*base, NULL, NULL, NULL);
		base++;
	}

	vfunc_state emptys = {0};
	arcan_mem_free(src->audb);

#ifndef _WIN32
	close(src->sockout_fd);
#endif

	arcan_audio_stop(src->aid);
	arcan_video_alterfeed(src->vid, NULL, emptys);

	arcan_event sevent = {.category = EVENT_FRAMESERVER,
		.kind = EVENT_FRAMESERVER_TERMINATED,
		.data.frameserver.video = src->vid,
		.data.frameserver.glsource = false,
		.data.frameserver.audio = src->aid,
		.data.frameserver.otag = src->tag
	};
	arcan_event_enqueue(arcan_event_defaultctx(), &sevent);

	arcan_mem_free(src);
	return ARCAN_OK;
}

/* won't do anything on windows */
void arcan_frameserver_dropsemaphores_keyed(char* key)
{
	char* work = strdup(key);
		work[ strlen(work) - 1] = 'v';
		arcan_sem_unlink(NULL, work);
		work[strlen(work) - 1] = 'a';
		arcan_sem_unlink(NULL, work);
		work[strlen(work) - 1] = 'e';
		arcan_sem_unlink(NULL, work);
	arcan_mem_free(work);
}

void arcan_frameserver_dropsemaphores(arcan_frameserver* src){
	if (src && src->shm.key && src->shm.ptr){
		arcan_frameserver_dropsemaphores_keyed(src->shm.key);
	}
}

bool arcan_frameserver_control_chld(arcan_frameserver* src){
/* bunch of terminating conditions -- frameserver messes
 * with the structure to provoke a vulnerability, frameserver
 * dying or timing out, ... */
	if ( src->flags.alive && src->shm.ptr &&
		src->shm.ptr->cookie == cookie &&
		arcan_frameserver_validchild(src) == false){

/* force flush beforehand, in a saturated queue, data may still
 * get lost here */
		arcan_event_queuetransfer(arcan_event_defaultctx(), &src->inqueue,
			src->queue_mask, 0.5, src->vid);

		arcan_frameserver_free(src);
		return false;
	}

	return true;
}

arcan_errc arcan_frameserver_pushevent(arcan_frameserver* dst,
	arcan_event* ev)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	if (!arcan_frameserver_enter(dst))
		return rv;

/*
 * NOTE: when arcan_event_serialize(*buffer) is implemented,
 * the queue should be stripped from the shmpage entirely and only
 * transferred over the socket(!)
 * The problem with the current approach is that we have no
 * decent mechanism active for waking a child that's simultaneously
 * polling and need to respond quickly to enqueued events
 */
	if (dst && ev){
		rv = dst->flags.alive && dst->shm.ptr ?
			(arcan_event_enqueue(&dst->outqueue, ev), ARCAN_OK) :
			ARCAN_ERRC_UNACCEPTED_STATE;

#ifndef _WIN32

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

		if (dst->flags.socksig){
			int sn = 0;
			send(dst->sockout_fd, &sn, sizeof(int), MSG_DONTWAIT);
		}
#endif
	}

	arcan_frameserver_leave();
	return rv;
}

static void push_buffer(arcan_frameserver* src,
	av_pixel* buf, struct storage_info_t* store)
{
	struct stream_meta stream = {.buf = NULL};
	size_t w = store->w;
	size_t h = store->h;

	if (src->flags.no_alpha_copy){
		av_pixel* wbuf = agp_stream_prepare(store, stream, STREAM_RAW).buf;
		if (!wbuf)
			return;

		size_t np = w * h;
		for (size_t i = 0; i < np; i++){
			av_pixel px = *buf++;
			*wbuf++ = RGBA_FULLALPHA_REPACK(px);
		}

		agp_stream_release(store);
	}
	else{
		stream.buf = buf;
		agp_stream_prepare(store, stream, src->flags.explicit ?
			STREAM_RAW_DIRECT_SYNCHRONOUS : STREAM_RAW_DIRECT);
	}

	agp_stream_commit(store);
}

enum arcan_ffunc_rv arcan_frameserver_dummyframe(
	enum arcan_ffunc_cmd cmd, uint8_t* buf,
	uint32_t s_buf, uint16_t width, uint16_t height, uint8_t bpp,
	unsigned mode, vfunc_state state)
{
    if (state.tag == ARCAN_TAG_FRAMESERV && state.ptr && cmd == FFUNC_DESTROY)
        arcan_frameserver_free( (arcan_frameserver*) state.ptr);

    return FFUNC_RV_NOFRAME;
}

enum arcan_ffunc_rv arcan_frameserver_emptyframe(
	enum arcan_ffunc_cmd cmd, av_pixel* buf,
	size_t s_buf, uint16_t width, uint16_t height,
	unsigned mode, vfunc_state state)
{
	arcan_frameserver* tgt = state.ptr;
	if (!tgt || state.tag != ARCAN_TAG_FRAMESERV
	 || !arcan_frameserver_enter(tgt))
		return FFUNC_RV_NOFRAME;

	switch (cmd){
		case FFUNC_POLL:

			if (tgt->shm.ptr->resized){
				arcan_frameserver_tick_control(tgt);
        if (tgt->shm.ptr && tgt->shm.ptr->vready){
					arcan_frameserver_leave();
        	return FFUNC_RV_GOTFRAME;
				}
			}

		case FFUNC_TICK:
			arcan_frameserver_tick_control(tgt);
		break;

		case FFUNC_DESTROY:
			arcan_frameserver_free(tgt);
		break;

		default:
			break;
	}

	arcan_frameserver_leave();
	return FFUNC_RV_NOFRAME;
}

static void check_audb(arcan_frameserver* tgt, struct arcan_shmif_page* shmpage)
{
/* interleave audio / video processing */
	if (!(shmpage->aready && shmpage->abufused))
		return;

	size_t ntc = tgt->ofs_audb + shmpage->abufused > tgt->sz_audb ?
		(tgt->sz_audb - tgt->ofs_audb) : shmpage->abufused;

	if (ntc == 0){
		static bool overflow;
		if (!overflow){
			arcan_warning("frameserver_videoframe_direct(), incoming buffer "
				"overflow for: %d, resetting.\n", tgt->vid);
			overflow = true;
		}
		tgt->ofs_audb = 0;
	}

	memcpy(&tgt->audb[tgt->ofs_audb], tgt->audp, ntc);
	tgt->ofs_audb += ntc;
	shmpage->abufused = 0;
	shmpage->aready = false;
	arcan_sem_post( tgt->async );
}

enum arcan_ffunc_rv arcan_frameserver_videoframe_direct(
	enum arcan_ffunc_cmd cmd, av_pixel* buf, size_t s_buf,
	uint16_t width, uint16_t height,
	unsigned int mode, vfunc_state state)
{
	int8_t rv = 0;
	if (state.tag != ARCAN_TAG_FRAMESERV || !state.ptr)
		return rv;

	arcan_frameserver* tgt = state.ptr;
	struct arcan_shmif_page* shmpage = tgt->shm.ptr;

	if (!shmpage || !arcan_frameserver_enter(tgt))
		return FFUNC_RV_NOFRAME;

	switch (cmd){
	case FFUNC_READBACK:
	break;

	case FFUNC_POLL:
		if (shmpage->resized){
			arcan_frameserver_tick_control(tgt);
			shmpage = tgt->shm.ptr;
		}

		check_audb(tgt, shmpage);

		rv = (tgt->playstate == ARCAN_PLAYING && shmpage->vready);
	break;

	case FFUNC_TICK:
		arcan_frameserver_tick_control( tgt );
	break;

	case FFUNC_DESTROY: arcan_frameserver_free( tgt ); break;

	case FFUNC_RENDER:
		arcan_event_queuetransfer(arcan_event_defaultctx(), &tgt->inqueue,
			tgt->queue_mask, 0.5, tgt->vid);

		push_buffer(tgt, tgt->vidp, arcan_video_getobject(tgt->vid)->vstore);

		if (tgt->desc.callback_framestate)
			emit_deliveredframe(tgt, shmpage->vpts, tgt->desc.framecount++);

		check_audb(tgt, shmpage);

/* interactive frameserver blocks on vsemaphore only,
 * so set monitor flags and wake up */
		shmpage->vready = false;
		arcan_sem_post( tgt->vsync );

		break;
  }

	arcan_frameserver_leave();
	return rv;
}

enum arcan_ffunc_rv arcan_frameserver_avfeedframe(
	enum arcan_ffunc_cmd cmd, av_pixel* buf,
	size_t s_buf, uint16_t width, uint16_t height,
	unsigned mode, vfunc_state state)
{
	assert(state.ptr);
	assert(state.tag == ARCAN_TAG_FRAMESERV);
	arcan_frameserver* src = (arcan_frameserver*) state.ptr;

	if (!arcan_frameserver_enter(src))
		return FFUNC_RV_NOFRAME;

	if (cmd == FFUNC_DESTROY)
		arcan_frameserver_free(state.ptr);

	else if (cmd == FFUNC_TICK){
/* done differently since we don't care if the frameserver wants
 * to resize, that's its problem. */
		if (!arcan_frameserver_control_chld(src)){
			arcan_frameserver_leave();
   		return FFUNC_RV_NOFRAME;
		}
	}

/*
 * if the frameserver isn't ready to receive (semaphore unlocked)
 * then the frame will be dropped, a warning noting that the
 * frameserver isn't fast enough to deal with the data (allowed to
 * duplicate frame to maintain framerate,
 * it can catch up reasonably by using less CPU intensive frame format.
 * Audio will keep on buffering until overflow,
 */
	else if (cmd == FFUNC_READBACK){
		if ( (src->flags.explicit && arcan_sem_wait(src->vsync) == 0) ||
			(!src->flags.explicit && arcan_sem_trywait(src->vsync) == 0)){
			memcpy(src->vidp, buf, s_buf);
			if (src->ofs_audb){
					memcpy(src->audp, src->audb, src->ofs_audb);
					src->shm.ptr->abufused = src->ofs_audb;
					src->ofs_audb = 0;
			}

/* it is possible that we deliver more videoframes than we can legitimately
 * encode in the target framerate, it is up to the frameserver
 * to determine when to drop and when to double frames */
			arcan_event ev  = {
				.kind = TARGET_COMMAND_STEPFRAME,
				.category = EVENT_TARGET,
				.data.target.ioevs[0] = src->vfcount++
			};

			arcan_event_enqueue(&src->outqueue, &ev);

			if (src->desc.callback_framestate)
				emit_deliveredframe(src, 0, src->desc.framecount++);
		}
		else {
			if (src->desc.callback_framestate)
				emit_droppedframe(src, 0, src->desc.dropcount++);
		}
	}
	else
			;

	arcan_frameserver_leave();
	return 0;
}

/* assumptions:
 * buf_sz doesn't contain partial samples (% (bytes per sample * channels))
 * dst->amixer inaud is allocated and allocation count matches n_aids */
static void feed_amixer(arcan_frameserver* dst, arcan_aobj_id srcid,
	int16_t* buf, int nsamples)
{
/* formats; nsamples (samples in, 2 samples / frame)
 * cur->inbuf; samples converted to float with gain, 2 samples / frame)
 * dst->outbuf; SINT16, in bytes, ofset in bytes */
	size_t minv = INT_MAX;

/* 1. Convert to float and buffer. Find the lowest common number of samples
 * buffered. Truncate if needed. Assume source feeds L/R */
	for (int i = 0; i < dst->amixer.n_aids; i++){
		struct frameserver_audsrc* cur = dst->amixer.inaud + i;

		if (cur->src_aid == srcid){
			int ulim = sizeof(cur->inbuf) / sizeof(float);
			int count = 0;

			while (nsamples-- && cur->inofs < ulim){
				float val = *buf++;
				cur->inbuf[cur->inofs++] =
					(count++ % 2 ? cur->l_gain : cur->r_gain) * (val / 32767.0f);
			}
		}

		if (cur->inofs < minv)
			minv = cur->inofs;
	}

/*
 * 2. If number of samples exceeds some threshold, mix (minv)
 * samples together and store in dst->outb Formulae used:
 * A = float(sampleA) * gainA.
 * B = float(sampleB) * gainB. Z = A + B - A * B
 */
		if (minv != INT_MAX && minv > 512 && dst->sz_audb - dst->ofs_audb > 0){
/* clamp */
			if (dst->ofs_audb + minv * sizeof(uint16_t) > dst->sz_audb)
				minv = (dst->sz_audb - dst->ofs_audb) / sizeof(uint16_t);

			for (int sc = 0; sc < minv; sc++){
				float work_sample = 0;

			for (int i = 0; i < dst->amixer.n_aids; i++){
				work_sample += dst->amixer.inaud[i].inbuf[sc] - (work_sample *
					dst->amixer.inaud[i].inbuf[sc]);
			}
/* clip output */
			int16_t sample_conv = work_sample >= 1.0 ? 32767.0 :
				(work_sample < -1.0 ? -32768 : work_sample * 32767);
			memcpy(&dst->audb[dst->ofs_audb], &sample_conv, sizeof(int16_t));
			dst->ofs_audb += sizeof(int16_t);
		}
/* 2b. Reset intermediate buffers, slide if needed. */
		for (int j = 0; j < dst->amixer.n_aids; j++){
			struct frameserver_audsrc* cur = dst->amixer.inaud + j;
			if (cur->inofs > minv){
				memmove(cur->inbuf, &cur->inbuf[minv], (cur->inofs - minv) *
					sizeof(float));
				cur->inofs -= minv;
			}
			else
				cur->inofs = 0;
		}
	}

}

void arcan_frameserver_update_mixweight(arcan_frameserver* dst,
	arcan_aobj_id src, float left, float right)
{
	for (int i = 0; i < dst->amixer.n_aids; i++){
		if (src == 0 || dst->amixer.inaud[i].src_aid == src){
			dst->amixer.inaud[i].l_gain = left;
			dst->amixer.inaud[i].r_gain = right;
		}
	}
}

void arcan_frameserver_avfeed_mixer(arcan_frameserver* dst, int n_sources,
	arcan_aobj_id* sources)
{
	assert(sources != NULL && dst != NULL && n_sources > 0);

	if (dst->amixer.n_aids)
		arcan_mem_free(dst->amixer.inaud);

	dst->amixer.inaud = arcan_alloc_mem(
		n_sources * sizeof(struct frameserver_audsrc),
		ARCAN_MEM_ATAG, ARCAN_MEM_BZERO, ARCAN_MEMALIGN_NATURAL);

	for (int i = 0; i < n_sources; i++){
		dst->amixer.inaud[i].l_gain  = 1.0;
		dst->amixer.inaud[i].r_gain  = 1.0;
		dst->amixer.inaud[i].inofs   = 0;
		dst->amixer.inaud[i].src_aid = *sources++;
	}

	dst->amixer.n_aids = n_sources;
}

void arcan_frameserver_avfeedmon(arcan_aobj_id src, uint8_t* buf,
	size_t buf_sz, unsigned channels, unsigned frequency, void* tag)
{
	arcan_frameserver* dst = tag;
	assert((intptr_t)(buf) % 4 == 0);

/*
 * with no mixing setup (lowest latency path), we just feed the sync buffer
 * shared with the frameserver. otherwise we forward to the amixer that is
 * responsible for pushing as much as has been generated by all the defined
 * sources
 */
	if (dst->amixer.n_aids > 0){
		feed_amixer(dst, src, (int16_t*) buf, buf_sz >> 1);
	}
	else if (dst->ofs_audb + buf_sz < dst->sz_audb){
			memcpy(dst->audb + dst->ofs_audb, buf, buf_sz);
			dst->ofs_audb += buf_sz;
	}
	else;
}

static inline void emit_deliveredframe(arcan_frameserver* src,
	unsigned long long pts, unsigned long long framecount)
{
	arcan_event deliv = {
		.category = EVENT_FRAMESERVER,
		.kind = EVENT_FRAMESERVER_DELIVEREDFRAME,
		.data.frameserver.pts = pts,
		.data.frameserver.counter = framecount,
		.data.frameserver.otag = src->tag,
		.data.frameserver.audio = src->aid,
		.data.frameserver.video = src->vid
	};

	arcan_event_enqueue(arcan_event_defaultctx(), &deliv);
}

static inline void emit_droppedframe(arcan_frameserver* src,
	unsigned long long pts, unsigned long long dropcount)
{
	arcan_event deliv = {
		.category = EVENT_FRAMESERVER,
		.kind = EVENT_FRAMESERVER_DROPPEDFRAME,
		.data.frameserver.pts = pts,
		.data.frameserver.counter = dropcount,
		.data.frameserver.otag = src->tag,
		.data.frameserver.audio = src->aid,
		.data.frameserver.video = src->vid
	};

	arcan_event_enqueue(arcan_event_defaultctx(), &deliv);
}

arcan_errc arcan_frameserver_audioframe_direct(arcan_aobj* aobj,
	arcan_aobj_id id, unsigned buffer, void* tag)
{
	arcan_errc rv = ARCAN_ERRC_NOTREADY;
	arcan_frameserver* src = (arcan_frameserver*) tag;

	if (buffer != -1 && src->audb && src->ofs_audb > ARCAN_ASTREAMBUF_LLIMIT){
/* this function will make sure all monitors etc. gets their chance */
			printf("arcan_audio_buffer\n");
			arcan_audio_buffer(aobj, buffer, src->audb, src->ofs_audb,
				src->desc.channels, src->desc.samplerate, tag);

		src->ofs_audb = 0;

		rv = ARCAN_OK;
	}

	return rv;
}

void arcan_frameserver_tick_control(arcan_frameserver* src)
{
	if (!arcan_frameserver_enter(src) ||
		!arcan_frameserver_control_chld(src) || !src || !src->shm.ptr)
		goto leave;

/* only allow the two categories below, and only let the
 * internal event queue be filled to half in order to not
 * have a crazy frameserver starve the main process */
	arcan_event_queuetransfer(arcan_event_defaultctx(), &src->inqueue,
		src->queue_mask, 0.5, src->vid);

	if (!src->shm.ptr->resized)
		goto leave;

	size_t neww = src->shm.ptr->w;
  size_t newh = src->shm.ptr->h;

	if (!arcan_frameserver_resize(&src->shm, neww, newh)){
 		arcan_warning("client requested illegal resize (%d, %d) -- killing.\n",
			neww, newh);
		arcan_frameserver_free(src);
		goto leave;
	}

/*
 * evqueues contain pointers into the shmpage that may have been moved
 */
	arcan_shmif_setevqs(src->shm.ptr, src->esync,
		&(src->inqueue), &(src->outqueue), true);

	struct arcan_shmif_page* shmpage = src->shm.ptr;
	/*
 * this is a rather costly operation that we want to rate-control or
 * at least monitor as multiple resizes in a short amount of time
 * is indicative of something foul going on.
 */
	vfunc_state cstate = *arcan_video_feedstate(src->vid);

/* resize the source vid in a way that won't propagate to user scripts
 * as we want the resize event to be forwarded to the regular callback */
	arcan_event_maskall(arcan_event_defaultctx());
	src->desc.samplerate = ARCAN_SHMPAGE_SAMPLERATE;
	src->desc.channels = ARCAN_SHMPAGE_ACHANNELS;

/*
 * resizefeed will also resize the underlying vstore
 */
	arcan_video_resizefeed(src->vid, neww, newh);

	arcan_event_clearmask(arcan_event_defaultctx());
	arcan_shmif_calcofs(shmpage, &(src->vidp), &(src->audp));

	arcan_video_alterfeed(src->vid, arcan_frameserver_videoframe_direct, cstate);

	arcan_event rezev = {
		.category = EVENT_FRAMESERVER,
		.kind = EVENT_FRAMESERVER_RESIZED,
		.data.frameserver.width = neww,
		.data.frameserver.height = newh,
		.data.frameserver.video = src->vid,
		.data.frameserver.audio = src->aid,
		.data.frameserver.otag = src->tag,
		.data.frameserver.glsource = shmpage->glsource
	};

	arcan_event_enqueue(arcan_event_defaultctx(), &rezev);

/* acknowledge the resize */
	shmpage->resized = false;

leave:
	arcan_frameserver_leave();
}

arcan_errc arcan_frameserver_pause(arcan_frameserver* src)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;

	if (src) {
		src->playstate = ARCAN_PAUSED;
		rv = ARCAN_OK;
	}

	return rv;
}

arcan_errc arcan_frameserver_resume(arcan_frameserver* src)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	if (src)
		src->playstate = ARCAN_PLAYING;

	return rv;
}

arcan_errc arcan_frameserver_flush(arcan_frameserver* fsrv)
{
	if (!fsrv)
		return ARCAN_ERRC_NO_SUCH_OBJECT;

	arcan_audio_rebuild(fsrv->aid);

	return ARCAN_OK;
}

arcan_frameserver* arcan_frameserver_alloc()
{
	arcan_frameserver* res = arcan_alloc_mem(sizeof(arcan_frameserver),
		ARCAN_MEM_VTAG, ARCAN_MEM_BZERO, ARCAN_MEMALIGN_NATURAL);

	if (!cookie)
		cookie = arcan_shmif_cookie();

	res->watch_const = 0xdead;

	res->playstate = ARCAN_PLAYING;
	res->flags.alive = true;

/* shm- related settings are deferred as this is called previous to mapping
 * (spawn_subsegment / spawn_server) so setting up the eventqueues with
 * killswitches have to be done elsewhere
 */

	return res;
}

void arcan_frameserver_configure(arcan_frameserver* ctx,
	struct frameserver_envp setup)
{
	arcan_errc errc;

	if (setup.use_builtin){
/* "libretro" (or rather, interactive mode) treats a single pair of
 * videoframe+audiobuffer each transfer, minimising latency is key. */
		if (strcmp(setup.args.builtin.mode, "libretro") == 0){
			ctx->aid = arcan_audio_feed((arcan_afunc_cb)
				arcan_frameserver_audioframe_direct, ctx, &errc);

			ctx->segid    = SEGID_GAME;
			ctx->sz_audb  = 1024 * 64;
			ctx->flags.socksig = false;
			ctx->ofs_audb = 0;
			ctx->segid = SEGID_GAME;
			ctx->audb     = arcan_alloc_mem(ctx->sz_audb,
				ARCAN_MEM_ABUFFER, 0, ARCAN_MEMALIGN_PAGE);
			ctx->queue_mask = EVENT_EXTERNAL;
		}

/* network client needs less in terms of buffering etc. but instead a
 * different signalling mechanism for flushing events */
		else if (strcmp(setup.args.builtin.mode, "net-cl") == 0){
			ctx->segid = SEGID_NETWORK_CLIENT;
			ctx->flags.socksig = true;
			ctx->queue_mask = EVENT_EXTERNAL | EVENT_NET;
		}
		else if (strcmp(setup.args.builtin.mode, "net-srv") == 0){
			ctx->segid = SEGID_NETWORK_SERVER;
			ctx->flags.socksig = true;
			ctx->queue_mask = EVENT_EXTERNAL | EVENT_NET;
		}

/* record instead operates by maintaining up-to-date local buffers,
 * then letting the frameserver sample whenever necessary */
		else if (strcmp(setup.args.builtin.mode, "record") == 0){
			ctx->segid = SEGID_ENCODER;

/* we don't know how many audio feeds are actually monitored to produce the
 * output, thus not how large the intermediate buffer should be to
 * safely accommodate them all */
			ctx->sz_audb = ARCAN_SHMPAGE_AUDIOBUF_SZ;
			ctx->audb = arcan_alloc_mem(ctx->sz_audb,
				ARCAN_MEM_ABUFFER, 0, ARCAN_MEMALIGN_PAGE);
			ctx->queue_mask = EVENT_EXTERNAL;
		}
		else {
			ctx->segid = SEGID_MEDIA;
			ctx->flags.socksig = true;
			ctx->aid = arcan_audio_feed(
			(arcan_afunc_cb) arcan_frameserver_audioframe_direct, ctx, &errc);
			ctx->sz_audb  = 1024 * 64;
			ctx->ofs_audb = 0;
			ctx->audb = arcan_alloc_mem(ctx->sz_audb,
				ARCAN_MEM_ABUFFER, 0, ARCAN_MEMALIGN_PAGE);
			ctx->queue_mask = EVENT_EXTERNAL;
		}
	}
/* hijack works as a 'process parasite' inside the rendering pipeline of
 * other projects, either through a generic fallback library or for
 * specialized "per- target" (in order to minimize size and handle 32/64
 * switching parent-vs-child relations */
	else{
		ctx->aid = arcan_audio_feed((arcan_afunc_cb)
			arcan_frameserver_audioframe_direct, ctx, &errc);
		ctx->hijacked = true;
		ctx->segid = SEGID_UNKNOWN;
		ctx->queue_mask = EVENT_EXTERNAL;

/* although audio playback tend to be kept in the child process, the
 * sampledata may still be needed for recording/monitoring */
		ctx->sz_audb  = 1024 * 64;
		ctx->ofs_audb = 0;
		ctx->audb = arcan_alloc_mem(ctx->sz_audb,
				ARCAN_MEM_ABUFFER, 0, ARCAN_MEMALIGN_PAGE);
	}

/* two separate queues for passing events back and forth between main program
 * and frameserver, set the buffer pointers to the relevant offsets in
 * backend_shmpage, and semaphores from the sem_open calls -- plan is
 * to switch this behavior on some platforms to instead use sockets to
 * improve I/O multiplexing (network- frameservers) or at least have futex
 * triggers on Linux */
	arcan_shmif_setevqs(ctx->shm.ptr, ctx->esync,
		&(ctx->inqueue), &(ctx->outqueue), true);
	ctx->inqueue.synch.killswitch = (void*) ctx;
	ctx->outqueue.synch.killswitch = (void*) ctx;

	struct arcan_shmif_page* shmpage = ctx->shm.ptr;
	shmpage->w = setup.init_w;
	shmpage->h = setup.init_h;

	arcan_shmif_calcofs(shmpage, &(ctx->vidp), &(ctx->audp));
}