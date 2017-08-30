/* Author: Romain "Artefact2" Dal Maso <artefact2@gmail.com>
 * 
 * This program is free software. It comes without any warranty, to the
 * extent permitted by applicable law. You can redistribute it and/or
 * modify it under the terms of the Do What The Fuck You Want To Public
 * License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>

#include <jack/jack.h>
#include <xmp.h>

static jack_client_t* client = NULL;
static jack_port_t* left = NULL;
static jack_port_t* right = NULL;
static unsigned int srate = 0;
static jack_nframes_t latency = 0;

static xmp_context xmpctx = NULL;
static struct xmp_frame_info xmpfinfo;
static size_t buffer_used = 0;

/* Source is s16 interleaved stereo samples */
static inline void convert_buffer(const int16_t* src, float* left, float* right, jack_nframes_t len) {	
	for(jack_nframes_t i = 0; i < len; ++i) {
		left[i]  = (float)src[2 * i]     / INT16_MAX;
		right[i] = (float)src[2 * i + 1] / INT16_MAX;
	}
}

static int jack_process(jack_nframes_t nframes, void* unused) {
	float* lbuf = jack_port_get_buffer(left, nframes);
	float* rbuf = jack_port_get_buffer(right, nframes);
	
	jack_nframes_t remaining = nframes;
	
	while(remaining > 0) {
		if(buffer_used + 4 * remaining < xmpfinfo.buffer_size) {
			/* Everything is already pre-generated */
			convert_buffer(&xmpfinfo.buffer[buffer_used], lbuf, rbuf, remaining);
			buffer_used += 4 * remaining;
			return 0;
		}
		
		/* Partial read from end of buffer then render next frame */
		jack_nframes_t towrite = (xmpfinfo.buffer_size - buffer_used) / 4;
		assert(towrite <= remaining);
		
		convert_buffer(&xmpfinfo.buffer[buffer_used], lbuf, rbuf, towrite);
		lbuf = &lbuf[towrite];
		rbuf = &rbuf[towrite];
		remaining -= towrite;
		xmp_play_frame(xmpctx);
		xmp_get_frame_info(xmpctx, &xmpfinfo);
		buffer_used = 0;
	}
	
	return 0;
}

static void jack_latency(jack_latency_callback_mode_t mode, void* unused) {
	if(mode != JackPlaybackLatency) return;
	if(client == NULL || left == NULL) return;

	jack_latency_range_t range;
	jack_port_get_latency_range(left, mode, &range);
	if(latency == range.max || range.max == 0) return;

	printf("\rJACK playback latency: %d/%d frames (%.2f/%.2f ms)\n",
		   range.min, range.max,
		   1000.f * range.min / srate, 1000.f * range.max / srate);
	latency = range.max;
}

static void usage(char* me) {
	fprintf(stderr, "Usage: %s <modfiles...>\n", me);
	exit(1);
}

int main(int argc, char** argv) {
	if(argc == 1) usage(argv[0]);
	
	client = jack_client_open("xmpjack", JackNullOption, NULL);
	if(client == NULL) return 1;

	printf("JACK client name: %s\n", jack_get_client_name(client));
	printf("JACK buffer size: %d frames\n", jack_get_buffer_size(client));
	printf("JACK sample rate: %d Hz\n", srate = jack_get_sample_rate(client));

	jack_set_process_callback(client, jack_process, NULL);
	jack_set_latency_callback(client, jack_latency, NULL);

	/* Default jack format: signed float */
	left = jack_port_register(client, "Left", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
	right = jack_port_register(client, "Right", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);

	printf("Creating xmp context, libxmp version %s.\n", xmp_version);
	xmpctx = xmp_create_context();
	for(int i = 1; i < argc; ++i) {
		printf("Playing back %s.\n", argv[i]);

		if(xmp_load_module(xmpctx, argv[i]) != 0) {
			fprintf(stderr, "Module %s could not be loaded by libxmp.\n", argv[i]);
			continue;
		}

		/* Default xmp sample format: s16 stereo interleaved */
		xmp_start_player(xmpctx, srate, 0);
		xmp_play_frame(xmpctx);
		xmp_get_frame_info(xmpctx, &xmpfinfo);
		
		/* XXX: make these user tuneable */
		xmp_set_player(xmpctx, XMP_PLAYER_AMP, 0);
		xmp_set_player(xmpctx, XMP_PLAYER_MIX, 100);
		xmp_set_player(xmpctx, XMP_PLAYER_INTERP, XMP_INTERP_NEAREST);		
		jack_activate(client);

		/* XXX */
		jack_connect(client, "xmpjack:Left", "system:playback_1");
		jack_connect(client, "xmpjack:Right", "system:playback_2");

		do {
			sleep(1);
		} while(xmpfinfo.loop_count == 0);

		jack_deactivate(client);
		xmp_end_player(xmpctx);
	}

	xmp_free_context(xmpctx);
	jack_client_close(client);
	return 0;
}

