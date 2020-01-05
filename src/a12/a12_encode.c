/*
 * Copyright: 2017-2019, Björn Ståhl
 * Description: A12 protocol state machine
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: https://arcan-fe.com
 */
#include <arcan_shmif.h>
#include <arcan_shmif_server.h>

#include <inttypes.h>
#include <string.h>
#include <math.h>

#include "a12.h"
#include "a12_int.h"
#include "a12_encode.h"

/*
 * create the control packet
 */
static void a12int_vframehdr_build(uint8_t buf[CONTROL_PACKET_SIZE],
	uint64_t last_seen, uint8_t chid,
	int type, uint32_t sid,
	uint16_t sw, uint16_t sh, uint16_t w, uint16_t h, uint16_t x, uint16_t y,
	uint32_t len, uint32_t exp_len, bool commit)
{
	a12int_trace(A12_TRACE_VDETAIL,
		"kind=header:ch=%"PRIu8":type=%d:stream=%"PRIu32
		":sw=%"PRIu16":sh=%"PRIu16":w=%"PRIu16":h=%"PRIu16":x=%"PRIu16
		":y=%"PRIu16":len=%"PRIu32":exp_len=%"PRIu32,
		chid, type, sid, sw, sh, w, h, x, y, len, exp_len
	);

	memset(buf, '\0', CONTROL_PACKET_SIZE);
	pack_u64(last_seen, &buf[0]);
/* uint8_t entropy[8]; */
	buf[16] = chid; /* [16] : channel-id */
	buf[17] = COMMAND_VIDEOFRAME; /* [17] : command */
	pack_u32(0, &buf[18]); /* [18..21] : stream-id */
	buf[22] = type; /* [22] : type */
	pack_u16(sw, &buf[23]); /* [23..24] : surfacew */
	pack_u16(sh, &buf[25]); /* [25..26] : surfaceh */
	pack_u16(x, &buf[27]); /* [27..28] : startx */
	pack_u16(y, &buf[29]); /* [29..30] : starty */
	pack_u16(w, &buf[31]); /* [31..32] : framew */
	pack_u16(h, &buf[33]); /* [33..34] : frameh */
	pack_u32(len, &buf[36]); /* [36..39] : length */
	pack_u32(exp_len, &buf[40]); /* [40..43] : exp-length */

/* [35] : dataflags: uint8 */
/* [40] Commit on completion, this is always set right now but will change
 * when 'chain of deltas' mode for shmif is added */
	buf[44] = commit;
}

/*
 * Need to chunk up a binary stream that do not have intermediate headers, that
 * typically comes with the compression / h264 / ...  output. To avoid yet
 * another copy, we use the prepend mechanism in a12int_append_out.
 */
static void chunk_pack(struct a12_state* S, int type,
	uint8_t chid, uint8_t* buf, size_t buf_sz, size_t chunk_sz)
{
	size_t n_chunks = buf_sz / chunk_sz;

	uint8_t outb[a12int_header_size(type)];
	outb[0] = chid; /* [0] : channel id */
	pack_u32(0xbacabaca, &outb[1]); /* [1..4] : stream */
	pack_u16(chunk_sz, &outb[5]); /* [5..6] : length */

	for (size_t i = 0; i < n_chunks; i++){
		a12int_append_out(S, type, &buf[i * chunk_sz], chunk_sz, outb, sizeof(outb));
	}

	size_t left = buf_sz - n_chunks * chunk_sz;
	pack_u16(left, &outb[5]); /* [5..6] : length */
	if (left)
		a12int_append_out(S, type, &buf[n_chunks * chunk_sz], left, outb, sizeof(outb));
}

void a12int_encode_araw(struct a12_state* S,
	uint8_t chid,
	shmif_asample* buf,
	uint16_t n_samples,
	struct a12_aframe_cfg cfg,
	struct a12_aframe_opts opts, size_t chunk_sz)
{
/* repack the audio into a temporary buffer for format reasons */
	size_t hdr_sz = a12int_header_size(STATE_AUDIO_PACKET);
	size_t buf_sz = hdr_sz + n_samples * sizeof(uint16_t) * cfg.channels;
	uint8_t* outb = malloc(hdr_sz + buf_sz);;
	if (!outb){
		a12int_trace(A12_TRACE_ALLOC,
			"failed to alloc %zu for s16aud", buf_sz);
		return;
	}

/* audio control message header */
	outb[16] = chid;
	outb[17] = COMMAND_AUDIOFRAME;
	pack_u32(0, &outb[18]); /* stream-id */
	outb[22] = cfg.channels; /* channels */
	outb[23] = 0; /* encoding, u16 */
	pack_u16(n_samples, &outb[24]);

/* repack into the right format (note, need _Generic on asample) */
	size_t pos = hdr_sz;
	for (size_t i = 0; i < n_samples; i++, pos += 2){
		pack_s16(buf[i], &outb[pos]);
	}

/* then split it up (though likely we get fed much smaller chunks) */
	a12int_append_out(S,
		STATE_CONTROL_PACKET, outb, CONTROL_PACKET_SIZE, NULL, 0);
	chunk_pack(S, STATE_AUDIO_PACKET, chid, &outb[hdr_sz], pos - hdr_sz, chunk_sz);
	free(outb);
}

/*
 * the rgb565, rgb and rgba function all follow the same pattern
 */
void a12int_encode_rgb565(PACK_ARGS)
{
	size_t px_sz = 2;

/* calculate chunk sizes based on a fitting amount of pixels */
	size_t hdr_sz = a12int_header_size(STATE_VIDEO_PACKET);
	size_t ppb = (chunk_sz - hdr_sz) / px_sz;
	size_t bpb = ppb * px_sz;
	size_t blocks = w * h / ppb;

	shmif_pixel* inbuf = vb->buffer;
	size_t pos = y * vb->pitch + x;

/* get the packing buffer, cancel if oom */
	uint8_t* outb = malloc(hdr_sz + bpb);
	if (!outb){
		a12int_trace(A12_TRACE_ALLOC,
			"failed to alloc %zu for rgb565", hdr_sz + bpb);
		return;
	}

/* store the control frame that defines our video buffer */
	uint8_t hdr_buf[CONTROL_PACKET_SIZE];
	a12int_vframehdr_build(hdr_buf, S->last_seen_seqnr, chid,
		POSTPROCESS_VIDEO_RGB565, 0, vb->w, vb->h, w, h, x, y,
		w * h * px_sz, w * h * px_sz, 1
	);
	a12int_append_out(S,
		STATE_CONTROL_PACKET, hdr_buf, CONTROL_PACKET_SIZE, NULL, 0);

	outb[0] = chid; /* [0] : channel id */
	pack_u32(0xbacabaca, &outb[1]); /* [1..4] : stream */
	pack_u16(bpb, &outb[5]); /* [5..6] : length */

/* sweep the incoming frame, and pack maximum block size */
	size_t row_len = w;
	for (size_t i = 0; i < blocks; i++){
		for (size_t j = 0; j < bpb; j += px_sz){
			uint8_t r, g, b, ign;
			uint16_t px;
			SHMIF_RGBA_DECOMP(inbuf[pos++], &r, &g, &b, &ign);
			px =
				(((b >> 3) & 0x1f) << 0) |
				(((g >> 2) & 0x3f) << 5) |
				(((r >> 3) & 0x1f) << 11)
			;
			pack_u16(px, &outb[hdr_sz+j]);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}
		a12int_append_out(S, STATE_VIDEO_PACKET, outb, hdr_sz + bpb, NULL, 0);
	}

/* last chunk */
	size_t left = ((w * h) - (blocks * ppb)) * px_sz;
	if (left){
		pack_u16(left, &outb[5]);
		a12int_trace(A12_TRACE_VDETAIL, "small block of %zu bytes", left);
		for (size_t i = 0; i < left; i+= px_sz){
			uint8_t r, g, b, ign;
			uint16_t px;
			SHMIF_RGBA_DECOMP(inbuf[pos++], &r, &g, &b, &ign);
			px =
				(((b >> 3) & 0x1f) << 0) |
				(((g >> 2) & 0x3f) << 5) |
				(((r >> 3) & 0x1f) << 11)
			;
			pack_u16(px, &outb[hdr_sz+i]);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}
		a12int_append_out(S, STATE_VIDEO_PACKET, outb, left+hdr_sz, NULL, 0);
	}

	free(outb);
}

void a12int_encode_rgba(PACK_ARGS)
{
	size_t px_sz = 4;
	a12int_trace(A12_TRACE_VDETAIL, "kind=status:codec=rgba");

/* calculate chunk sizes based on a fitting amount of pixels */
	size_t hdr_sz = a12int_header_size(STATE_VIDEO_PACKET);
	size_t ppb = (chunk_sz - hdr_sz) / px_sz;
	size_t bpb = ppb * px_sz;
	size_t blocks = w * h / ppb;

	shmif_pixel* inbuf = vb->buffer;
	size_t pos = y * vb->pitch + x;

/* get the packing buffer, cancel if oom */
	uint8_t* outb = malloc(hdr_sz + bpb);
	if (!outb)
		return;

/* store the control frame that defines our video buffer */
	uint8_t hdr_buf[CONTROL_PACKET_SIZE];
	a12int_vframehdr_build(hdr_buf, S->last_seen_seqnr, chid,
		POSTPROCESS_VIDEO_RGBA, 0, vb->w, vb->h, w, h, x, y,
		w * h * px_sz, w * h * px_sz, 1
	);
	a12int_append_out(S,
		STATE_CONTROL_PACKET, hdr_buf, CONTROL_PACKET_SIZE, NULL, 0);

	outb[0] = chid; /* [0] : channel id */
	pack_u32(0xbacabaca, &outb[1]); /* [1..4] : stream */
	pack_u16(bpb, &outb[5]); /* [5..6] : length */

/* sweep the incoming frame, and pack maximum block size */
	size_t row_len = w;
	for (size_t i = 0; i < blocks; i++){
		for (size_t j = 0; j < bpb; j += px_sz){
			uint8_t* dst = &outb[hdr_sz+j];
			SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &dst[3]);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}

/* dispatch to out-queue(s) */
		a12int_append_out(S, STATE_VIDEO_PACKET, outb, hdr_sz + bpb, NULL, 0);
	}

/* last chunk */
	size_t left = ((w * h) - (blocks * ppb)) * px_sz;
	if (left){
		pack_u16(left, &outb[5]);
		a12int_trace(A12_TRACE_VDETAIL,
			"kind=status:message=padblock:size=%zu", left);
		for (size_t i = 0; i < left; i+= px_sz){
			uint8_t* dst = &outb[hdr_sz+i];
			SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &dst[3]);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}
		a12int_append_out(S, STATE_VIDEO_PACKET, outb, hdr_sz + left, NULL, 0);
	}

	free(outb);
}

void a12int_encode_rgb(PACK_ARGS)
{
	size_t px_sz = 3;
	a12int_trace(A12_TRACE_VDETAIL, "kind=status:ch=%"PRIu8"codec=rgb", (uint8_t) chid);

/* calculate chunk sizes based on a fitting amount of pixels */
	size_t hdr_sz = a12int_header_size(STATE_VIDEO_PACKET);
	size_t ppb = (chunk_sz - hdr_sz) / px_sz;
	size_t bpb = ppb * px_sz;
	size_t blocks = w * h / ppb;

	shmif_pixel* inbuf = vb->buffer;
	size_t pos = y * vb->pitch + x;

/* get the packing buffer, cancel if oom */
	uint8_t* outb = malloc(hdr_sz + bpb);
	if (!outb)
		return;

/* store the control frame that defines our video buffer */
	uint8_t hdr_buf[CONTROL_PACKET_SIZE];
	a12int_vframehdr_build(hdr_buf, S->last_seen_seqnr, chid,
		POSTPROCESS_VIDEO_RGB, 0, vb->w, vb->h, w, h, x, y,
		w * h * px_sz, w * h * px_sz, 1
	);
	a12int_append_out(S,
		STATE_CONTROL_PACKET, hdr_buf, CONTROL_PACKET_SIZE, NULL, 0);

	outb[0] = chid; /* [0] : channel id */
	pack_u32(0xbacabaca, &outb[1]); /* [1..4] : stream */
	pack_u16(bpb, &outb[5]); /* [5..6] : length */

/* sweep the incoming frame, and pack maximum block size */
	size_t row_len = w;
	for (size_t i = 0; i < blocks; i++){
		for (size_t j = 0; j < bpb; j += px_sz){
			uint8_t ign;
			uint8_t* dst = &outb[hdr_sz+j];
			SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &ign);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}

/* dispatch to out-queue(s) */
		a12int_append_out(S, STATE_VIDEO_PACKET, outb, hdr_sz + bpb, NULL, 0);
	}

/* last chunk */
	size_t left = ((w * h) - (blocks * ppb)) * px_sz;
	if (left){
		pack_u16(left, &outb[5]);
/* sweep the incoming frame, and pack maximum block size */
		size_t row_len = w;
		for (size_t i = 0; i < blocks; i++){
			for (size_t j = 0; j < bpb; j += px_sz){
				uint8_t ign;
				uint8_t* dst = &outb[hdr_sz+j];
				SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &ign);
				row_len--;
				if (row_len == 0){
					pos += vb->pitch - w;
					row_len = w;
				}
/* dispatch to out-queue(s) */
			}

			a12int_append_out(S, STATE_VIDEO_PACKET, outb, hdr_sz + left, NULL, 0);
		}
	}

	free(outb);
}

struct compress_res {
	bool ok;
	uint8_t type;
	size_t in_sz;
	size_t out_sz;
	uint8_t* out_buf;
};

static struct compress_res compress_tz(struct a12_state* S,
	uint8_t ch, struct shmifsrv_vbuffer* vb)
{
/* full header-size: 4 + 2 + 2 + 1 + 2 + 4 + 1 = 16 bytes */
/* first 4 bytes is length */
	uint32_t compress_in_sz;
	unpack_u32(&compress_in_sz, vb->buffer_bytes);

/* second 2 bytes is number of lines (line-header size) */
	uint16_t n_lines;
	unpack_u16(&n_lines, &vb->buffer_bytes[4]);

/* third 2 bytes is number of cells */
	uint16_t n_cells;
	unpack_u16(&n_cells, &vb->buffer_bytes[6]);

/* line-header size (2 + 2 + 2 + 3 = 9 bytes), cell size = 12 bytes) */
	if (compress_in_sz != n_lines * 9 + n_cells * 12 + 16){
		a12int_trace(A12_TRACE_SYSTEM, "kind=error:message=corrupt TPACK buffer");
		return (struct compress_res){};
	}

/* all the cell attributes and colors etc. lend themselves well to
 * compression, so lets go ahead with miniz */
	size_t out_sz;
	uint8_t* buf = tdefl_compress_mem_to_heap(
		vb->buffer_bytes, compress_in_sz, &out_sz, 0);

	if (!buf){
		a12int_trace(A12_TRACE_ALLOC, "failed to build compressed TPACK output");
		return (struct compress_res){};
	}

	return (struct compress_res){
		.type = POSTPROCESS_VIDEO_TZ,
		.ok = true,
		.out_buf = buf,
		.out_sz = out_sz,
		.in_sz = compress_in_sz
	};
}

void a12int_encode_tz(PACK_ARGS)
{
	struct compress_res cres = compress_tz(S, chid, vb);
	if (!cres.ok)
		return;

	uint8_t hdr_buf[CONTROL_PACKET_SIZE];
	a12int_vframehdr_build(hdr_buf, S->last_seen_seqnr, chid,
		cres.type, 0, vb->w, vb->h, w, h, 0, 0,
		cres.out_sz, cres.in_sz, 1
	);

	a12int_trace(A12_TRACE_VDETAIL,
		"kind=status:codec=tpack:b_in=%zu:b_out=%zu", cres.in_sz, cres.out_sz
	);

	a12int_append_out(S,
		STATE_CONTROL_PACKET, hdr_buf, CONTROL_PACKET_SIZE, NULL, 0);
	chunk_pack(S, STATE_VIDEO_PACKET, chid, cres.out_buf, cres.out_sz, chunk_sz);

	free(cres.out_buf);
}

static struct compress_res compress_deltaz(struct a12_state* S, uint8_t ch,
	struct shmifsrv_vbuffer* vb, size_t* x, size_t* y, size_t* w, size_t* h)
{
	int type;
	uint8_t* compress_in;
	size_t compress_in_sz = 0;
	struct shmifsrv_vbuffer* ab = &S->channels[ch].acc;

/* reset the accumulation buffer so that we rebuild the normal frame */
	if (ab->w != vb->w || ab->h != vb->h){
		a12int_trace(A12_TRACE_VIDEO,
			"kind=resize:ch=%"PRIu8"prev_w=%zu:rev_h=%zu:new_w%zu:new_h=%zu",
			ch, (size_t) ab->w, (size_t) ab->h, (size_t) vb->w, (size_t) vb->h
		);
		free(ab->buffer);
		free(S->channels[ch].compression);
		ab->buffer = NULL;
		S->channels[ch].compression = NULL;
	}

/* first, reset or no-delta mode, build accumulation buffer and copy */
	if (!ab->buffer){
		type = POSTPROCESS_VIDEO_MINIZ;
		*ab = *vb;
		size_t nb = vb->w * vb->h * 3;
		ab->buffer = malloc(nb);
		*w = vb->w;
		*h = vb->h;
		*x = 0;
		*y = 0;
		a12int_trace(A12_TRACE_VIDEO,
			"kind=status:ch=%"PRIu8"compress=dpng:message=I", ch);

		if (!ab->buffer)
			return (struct compress_res){};

/* the compression buffer stores a ^ b, accumulation is a packed copy of the
 * contents of the previous input frame, this should provide a better basis for
 * deflates RLE etc. stages, but also act as an option for us to provide our
 * cheaper RLE or send out a raw- frame when the RLE didn't work out */
		S->channels[ch].compression = malloc(nb);
		compress_in_sz = nb;

		if (!S->channels[ch].compression){
			free(ab->buffer);
			ab->buffer = NULL;
			return (struct compress_res){};
		}

/* so accumulation buffer might be tightly packed while the source
 * buffer do not have to be, thus we need to iterate and do this copy */
		compress_in = (uint8_t*) ab->buffer;
		uint8_t* acc = compress_in;
		size_t ofs = 0;
		for (size_t y = 0; y < vb->h; y++){
			for (size_t x = 0; x < vb->w; x++){
				uint8_t ign;
				shmif_pixel px = vb->buffer[y*vb->pitch+x];
				SHMIF_RGBA_DECOMP(px, &acc[ofs], &acc[ofs+1], &acc[ofs+2], &ign);
				ofs += 3;
			}
		}
	}
/* We have a delta frame, use accumulation buffer as a way to calculate a ^ b
 * and store ^ b. For smaller regions, we might want to do something simpler
 * like RLE only. The flags (,0) can be derived with the _zip helper */
	else {
		a12int_trace(A12_TRACE_VDETAIL,
			"kind=status:ch=%"PRIu8"dw=%zu:dh=%zu:x=%zu:y=%zu",
			ch, (size_t)*w, (size_t)*h, (size_t) *x, (size_t) *y
		);
		compress_in = S->channels[ch].compression;
		uint8_t* acc = (uint8_t*) ab->buffer;
		for (size_t cy = (*y); cy < (*y)+(*h); cy++){
			size_t rs = (cy * ab->w + (*x)) * 3;

			for (size_t cx = *x; cx < (*x)+(*w); cx++){
				uint8_t r, g, b, ign;
				shmif_pixel px = vb->buffer[cy * vb->pitch + cx];
				SHMIF_RGBA_DECOMP(px, &r, &g, &b, &ign);
				compress_in[compress_in_sz++] = acc[rs+0] ^ r;
				compress_in[compress_in_sz++] = acc[rs+1] ^ g;
				compress_in[compress_in_sz++] = acc[rs+2] ^ b;
				acc[rs+0] = r; acc[rs+1] = g; acc[rs+2] = b;
				rs += 3;
			}
		}
		type = POSTPROCESS_VIDEO_DMINIZ;
	}

	size_t out_sz;

	uint8_t* buf = tdefl_compress_mem_to_heap(
			compress_in, compress_in_sz, &out_sz, 0);

	return (struct compress_res){
		.type = type,
		.ok = buf != NULL,
		.out_buf = buf,
		.out_sz = out_sz,
		.in_sz = compress_in_sz
	};
}

void a12int_encode_dpng(PACK_ARGS)
{
	struct compress_res cres = compress_deltaz(S, chid, vb, &x, &y, &w, &h);
	if (!cres.ok)
		return;

	uint8_t hdr_buf[CONTROL_PACKET_SIZE];
	a12int_vframehdr_build(hdr_buf, S->last_seen_seqnr, chid,
		cres.type, 0, vb->w, vb->h, w, h, x, y,
		cres.out_sz, cres.in_sz, 1
	);

	a12int_trace(A12_TRACE_VDETAIL,
		"kind=status:codec=dpng:b_in=%zu:b_out=%zu", w * h * 3, cres.out_sz
	);

	a12int_append_out(S,
		STATE_CONTROL_PACKET, hdr_buf, CONTROL_PACKET_SIZE, NULL, 0);
	chunk_pack(S, STATE_VIDEO_PACKET, chid, cres.out_buf, cres.out_sz, chunk_sz);

	free(cres.out_buf);
}

#if defined(WANT_H264_ENC) || defined(WANT_H264_DEC)
void a12int_drop_videnc(struct a12_state* S, int chid, bool failed)
{
	if (!S->channels[chid].videnc.encoder)
		return;

/* dealloc context */
	S->channels[chid].videnc.encoder = NULL;
	S->channels[chid].videnc.failed = failed;

	if (S->channels[chid].videnc.scaler){
		sws_freeContext(S->channels[chid].videnc.scaler);
		S->channels[chid].videnc.scaler = NULL;
	}

	if (S->channels[chid].videnc.frame){
		av_frame_free(&S->channels[chid].videnc.frame);
	}

/* free both sets NULL and noops on NULL */
	av_packet_free(&S->channels[chid].videnc.packet);

	a12int_trace(A12_TRACE_VIDEO, "dropping h264 context");
}

static unsigned long pick_bitrate(size_t w, size_t h, struct a12_vframe_opts o)
{
/* Just some rough 'better than nothing' table for when we don't get a CRF or a
 * specified bitrate by the caller during setup or through a later backpressure
 * estimation */
	return 1000000;
}

static bool open_videnc(struct a12_state* S,
	struct a12_vframe_opts venc_opts,
	struct shmifsrv_vbuffer* vb, int chid, int codecid)
{
	a12int_trace(A12_TRACE_VIDEO,
		"kind=codec:status=open:ch=%d:codec=%d", chid, codecid);
	AVCodec* codec = S->channels[chid].videnc.codec;
	AVFrame* frame = NULL;
	AVPacket* packet = NULL;
	struct SwsContext* scaler = NULL;

	if (!codec){
		codec = avcodec_find_encoder(codecid);
		if (!codec)
			return false;
		S->channels[chid].videnc.codec = codec;
	}

/*
 * prior to this, we have a safeguard if the input resolution isn't % 2 so
 * this requirement for ffmpeg holds
 */
	AVCodecContext* encoder = avcodec_alloc_context3(codec);
	S->channels[chid].videnc.encoder = encoder;
	S->channels[chid].videnc.w = vb->w;
	S->channels[chid].videnc.h = vb->h;

/* Check opts and switch preset, bitrate, tuning etc. based on resolution
 * and link estimates. Later we should switch this dynamically, possibly
 * reconfigure based on AV_CODEC_CAP_PARAM_CHANGE */
	if (codecid == AV_CODEC_ID_H264){
		switch(venc_opts.bias){
		case VFRAME_BIAS_LATENCY:
			av_opt_set(encoder->priv_data, "preset", "veryfast", 0);
			av_opt_set(encoder->priv_data, "tune", "zerolatency", 0);
		break;

/* Many more dynamic heuristics to consider here, doing rolling frame contents
 * based on segment type and to distinguish GAME based on the complexity
 * (retro/pixelart vs. 3D) and on the load */
		case VFRAME_BIAS_BALANCED:
			av_opt_set(encoder->priv_data, "preset", "medium", 0);
			av_opt_set(encoder->priv_data, "tune", "film", 0);
		break;

		case VFRAME_BIAS_QUALITY:
			av_opt_set(encoder->priv_data, "preset", "slow", 0);
			av_opt_set(encoder->priv_data, "tune", "film", 0);
		break;
		}
	}

/* should expose a lot more options passable from the transport layer here,
 * but that's something for the 0.6 series */
	if (venc_opts.variable){
	}
	else {
		encoder->bit_rate = venc_opts.bitrate > 0 ?
			(venc_opts.bitrate * 1000000.0f) : pick_bitrate(vb->w, vb->h, venc_opts);
	}
	encoder->width = vb->w;
	encoder->height = vb->h;

/* uncertain about the level of VFR support, but that's really what we need
 * and then possibly abuse the PTS field to prebuffer frames in the context
 * of video playback and so on. */
	encoder->time_base = (AVRational){1, 25};
	encoder->framerate = (AVRational){25, 1};
	encoder->gop_size = 1;
	encoder->max_b_frames = 1;
	encoder->pix_fmt = AV_PIX_FMT_YUV420P;
	if (avcodec_open2(encoder, codec, NULL) < 0)
		goto fail;

	frame = av_frame_alloc();
	if (!frame)
		goto fail;

	packet = av_packet_alloc();
	if (!packet)
		goto fail;

	frame->format = AV_PIX_FMT_YUV420P;
	frame->width = vb->w;
	frame->height = vb->h;
	frame->pts = 0;

	if (av_frame_get_buffer(frame, 32) < 0 ||
		av_frame_make_writable(frame) < 0)
		goto fail;

	S->channels[chid].videnc.encoder = encoder;

	scaler = sws_getContext(
		vb->w, vb->h, AV_PIX_FMT_BGRA,
		vb->w, vb->h, AV_PIX_FMT_YUV420P,
		SWS_BILINEAR, NULL, NULL, NULL
	);

	if (!scaler)
		goto fail;

	S->channels[chid].videnc.scaler = scaler;
	S->channels[chid].videnc.frame = frame;
	S->channels[chid].videnc.packet = packet;

	a12int_trace(A12_TRACE_VIDEO, "kind=codec_ok:ch=%d:codec=%d", chid, codecid);
	return true;

fail:
	if (frame)
		av_frame_free(&frame);
	if (packet)
		av_packet_free(&packet);
	if (scaler)
		sws_freeContext(scaler);
	a12int_trace(A12_TRACE_SYSTEM, "kind=error:message=could not setup codec");
	return false;
}
#endif

void a12int_encode_h264(PACK_ARGS)
{
/* A major complication here is that there is a requirement for the
 * source- width and height to be evenly divisible by 2. The option
 * then is to pad, or the cheap fallback of switching codec. Let us
 * go with the cheap one for now. */
#ifdef WANT_H264_ENC
	if (vb->w % 2 != 0 || vb->h % 2 != 0){
		a12int_drop_videnc(S, chid, true);
	}

/* On resize, rebuild the encoder stage and send new headers etc. */
	else if (
		vb->w != S->channels[chid].videnc.w ||
		vb->h != S->channels[chid].videnc.h)
		a12int_drop_videnc(S, chid, false);

/* If we don't have an encoder (first time or reset due to resize),
 * try to configure, and if the configuration fails (i.e. still no
 * encoder set) fallback to DPNG and only try again on new size. */
	if (!S->channels[chid].videnc.encoder &&
			!S->channels[chid].videnc.failed){
		if (!open_videnc(S, opts, vb, chid, AV_CODEC_ID_H264)){
			a12int_trace(A12_TRACE_SYSTEM, "kind=error:message=h264 codec failed");
			a12int_drop_videnc(S, chid, true);
		}
		else
			a12int_trace(A12_TRACE_VIDEO, "kind=status:ch=%d:message=set-h264", chid);
	}

/* on failure, just fallback and retry alloc on dimensions change */
	if (S->channels[chid].videnc.failed)
		goto fallback;

/* just for shorthand */
	AVFrame* frame = S->channels[chid].videnc.frame;
	AVCodecContext* encoder = S->channels[chid].videnc.encoder;
	AVPacket* packet = S->channels[chid].videnc.packet;
	struct SwsContext* scaler = S->channels[chid].videnc.scaler;

/* and color-convert from src into frame */
	int ret;
	const uint8_t* const src[] = {(uint8_t*)vb->buffer};
	int src_stride[] = {vb->stride};
	int rv = sws_scale(scaler,
		src, src_stride, 0, vb->h, frame->data, frame->linesize);
	if (rv < 0){
		a12int_trace(A12_TRACE_VIDEO, "rescaling failed: %d", rv);
		a12int_drop_videnc(S, chid, true);
		goto fallback;
	}

/* send to encoder, may return EAGAIN requesting a flush */
again:
	ret = avcodec_send_frame(encoder, frame);
	if (ret < 0 && ret != AVERROR(EAGAIN)){
		a12int_trace(A12_TRACE_VIDEO, "encoder failed: %d", ret);
		a12int_drop_videnc(S, chid, true);
		goto fallback;
	}

/* flush, 0 is OK, < 0 and not EAGAIN is a real error */
	int out_ret;
	do {
		out_ret = avcodec_receive_packet(encoder, packet);
		if (out_ret == AVERROR(EAGAIN) || out_ret == AVERROR_EOF)
			return;

		else if (out_ret < 0){
			a12int_trace(
				A12_TRACE_VIDEO, "error getting packet from encoder: %d", rv);
			a12int_drop_videnc(S, chid, true);
			goto fallback;
		}

		a12int_trace(A12_TRACE_VDETAIL, "videnc: %5d", packet->size);

/* don't see a nice way to combine ffmpegs view of 'packets' and ours,
 * maybe we could avoid it and the extra copy but uncertain */
		uint8_t hdr_buf[CONTROL_PACKET_SIZE];
		a12int_vframehdr_build(hdr_buf, S->last_seen_seqnr, chid,
			POSTPROCESS_VIDEO_H264, 0, vb->w, vb->h, vb->w, vb->h,
			0, 0, packet->size, vb->w * vb->h * 4, 1
		);
		a12int_append_out(S,
			STATE_CONTROL_PACKET, hdr_buf, CONTROL_PACKET_SIZE, NULL, 0);

		chunk_pack(S, STATE_VIDEO_PACKET, chid, packet->data, packet->size, chunk_sz);
		av_packet_unref(packet);
		frame->pts++;
	}
	while (out_ret >= 0);

/* frame never got encoded, should work now */
	if (ret == AVERROR(EAGAIN))
		goto again;

	return;

fallback:
	a12int_encode_dpng(FWD_ARGS);
#else
	a12int_encode_dpng(FWD_ARGS);
#endif
	a12int_trace(A12_TRACE_VIDEO, "switching to fallback (PNG) on videnc fail");
}
