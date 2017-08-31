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
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <jack/jack.h>
#include <xmp.h>

#include <sys/select.h>
#include <termios.h>

/* +20dB = x10, +1 dB = 10^.05 */
static float one_db = 1.12201845430196343559f;

static const char* const Notes[] = { "A-", "A#", "B-", "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#" };
static const char* const notes[] = { "a-", "a#", "b-", "c-", "c#", "d-", "d#", "e-", "f-", "f#", "g-", "g#" };

static jack_client_t* client = NULL;
static jack_port_t* left = NULL;
static jack_port_t* right = NULL;
static unsigned int srate = 0;
static jack_nframes_t latency = 0;

static xmp_context xmpctx = NULL;
static struct xmp_frame_info xmpfinfo;
static size_t buffer_used = 0;
static bool new_frame = true;

static bool paused = false;
static unsigned int prev_loop_count;
static unsigned int loop = false;
static int gain_db = 0;
static float gain_mul = 1.f;
static jack_time_t notif_until = 0;

static struct termios cflags, pflags;

/* Source is s16 interleaved stereo samples */
static inline void convert_buffer(const int16_t* src, float* left, float* right, jack_nframes_t len) {	
	for(jack_nframes_t i = 0; i < len; ++i) {
		left[i]  = (float)src[2 * i]     / INT16_MAX * gain_mul;
		right[i] = (float)src[2 * i + 1] / INT16_MAX * gain_mul;
	}
}

static inline void render_frame(void) {
	xmp_play_frame(xmpctx);
	xmp_get_frame_info(xmpctx, &xmpfinfo);
	buffer_used = 0;
	new_frame = true;
}

static inline void clear_vis(void) {
	printf("\r%*c\r", 2 * XMP_MAX_CHANNELS, ' ');
}

static void restore_term(void) {
	tcsetattr(0, TCSANOW, &pflags);
}

static char get_command(void) {
	static fd_set f;
	static struct timeval t;
	
	/* select() on stdin for reading, do not block */
	FD_ZERO(&f);
	FD_SET(fileno(stdin), &f);
	t.tv_sec = 0;
	t.tv_usec = 0;

	if(select(1, &f, NULL, NULL, &t) > 0) return getchar();
	return 0;
}

static int jack_process(jack_nframes_t nframes, void* unused) {
	float* lbuf = jack_port_get_buffer(left, nframes);
	float* rbuf = jack_port_get_buffer(right, nframes);

	if(paused) {
		memset(lbuf, 0, nframes * sizeof(float));
		memset(rbuf, 0, nframes * sizeof(float));
		return 0;
	}
	
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

		render_frame();
	}
	
	return 0;
}

static void jack_latency(jack_latency_callback_mode_t mode, void* unused) {
	if(mode != JackPlaybackLatency) return;
	if(client == NULL || left == NULL) return;

	jack_latency_range_t range;
	jack_port_get_latency_range(left, mode, &range);
	if(latency == range.max || range.max == 0) return;

	clear_vis();
	printf("JACK playback latency: %d/%d frames (%.2f/%.2f ms)\n",
		   range.min, range.max,
		   1000.f * range.min / srate, 1000.f * range.max / srate);
	latency = range.max;
}

static void usage(FILE* to, char* me) {
	fprintf(
		to,
		"Usage: %s <modfiles...>\n"
		"\n"
		"Interactive commands:\n"
		"\tq\tQuit the program\n"
		"\tSPACE\tToggle play/pause\n"
		"\tn\tPlay next module\n"
		"\tp\tPlay previous module\n"
		"\t/*\tIncrease/decrease gain by 1 dB\n"
		"\n"
		, me);
}

static void print_notif(const char *fmt, ...) {
	va_list ap;

	notif_until = jack_get_time() + 1000000;
	clear_vis();

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	
	fflush(stdout);
}

static void print_vis(void) {
	if(jack_get_time() < notif_until) return;
	
	putchar('\r');
	
	for(size_t j = 0; j < XMP_MAX_CHANNELS; ++j) {
		struct xmp_channel_info* info = &xmpfinfo.channel_info[j];

		if(info->period == 0 || info->volume == 0) {
			fputs("  ", stdout);
		} else {
			size_t note = (size_t)roundf((fmodf(log2f(1.f / info->period), 1.f) + 1.f) * 12.f) % 12;
			
			printf("%c[%d%sm%s%c[0m",
				   27,
				   31 + (info->instrument % 6),
				   (info->volume >= 40) ? ";1" : "",
				   (info->volume >= 20) ? Notes[note] : notes[note],
				   27);
		}
	}
	fflush(stdout);
}

int main(int argc, char** argv) {
	if(argc == 1) {
		usage(stderr, argv[0]);
		exit(1);
	}

	atexit(restore_term);
	tcgetattr(0, &pflags);
	cflags = pflags;
	cflags.c_lflag &= ~ECHO;
	cflags.c_lflag &= ~ICANON;
	cflags.c_lflag |= ECHONL;
	tcsetattr(0, TCSANOW, &cflags);
	
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
		clear_vis();
		printf("Playing back %s.\n", argv[i]);

		if(xmp_load_module(xmpctx, argv[i]) != 0) {
			fprintf(stderr, "Module %s could not be loaded by libxmp.\n", argv[i]);
			continue;
		}

		/* Default xmp sample format: s16 stereo interleaved */
		xmp_start_player(xmpctx, srate, 0);
		render_frame();
		prev_loop_count = 0;
		
		/* XXX: make these user tuneable */
		xmp_set_player(xmpctx, XMP_PLAYER_AMP, 0);
		xmp_set_player(xmpctx, XMP_PLAYER_MIX, 100);
		xmp_set_player(xmpctx, XMP_PLAYER_INTERP, XMP_INTERP_NEAREST);		
		jack_activate(client);

		/* XXX */
		jack_connect(client, "xmpjack:Left", "system:playback_1");
		jack_connect(client, "xmpjack:Right", "system:playback_2");

		for(;;) {
			if(new_frame) {
				new_frame = false;

				if(!loop && prev_loop_count != xmpfinfo.loop_count) break;
				else prev_loop_count = xmpfinfo.loop_count;

				print_vis();
			}

			switch(get_command()) {
			case 'q':
				i = argc;
				goto end;
				break;

			case ' ':
				paused = !paused;
				print_notif("Pause: %s", paused ? "ON" : "OFF");
				break;

			case 'l':
				loop = !loop;
				print_notif("Looping: %s", loop ? "ON" : "OFF");
				break;

			case 'n':
				goto end;
				break;

			case 'p':
				i -= 2;
				goto end;
				break;

			case 'h':
				clear_vis();
				usage(stdout, argv[0]);
				break;

			case '/':
				gain_db -= 2;
				gain_mul /= one_db;
				gain_mul /= one_db;
			case '*':
				++gain_db;
				gain_mul *= one_db;
				print_notif("Gain: %+d dB", gain_db);
				break;
				
			case 0:
			default:
				break;
			}
			
			usleep(10000);
		}

	end:
		jack_deactivate(client);
		xmp_end_player(xmpctx);
	}

	xmp_free_context(xmpctx);
	jack_client_close(client);
	clear_vis();
	printf("Exiting.\n");
	return 0;
}

