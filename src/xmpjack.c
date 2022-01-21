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
#include <stdarg.h>
#include <unistd.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <pthread.h>
#include <jack/jack.h>
#include <xmp.h>

#include <sys/select.h>
#include <termios.h>

/* +20dB = x10, +1 dB = 10^.05 */
static float one_db = 1.12201845430196343559f;

#define EOL "\e[0K\n"
static const char* const Notes[] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-", };
static const char* const notes[] = { "c-", "c#", "d-", "d#", "e-", "f-", "f#", "g-", "g#", "a-", "a#", "b-", };
static const int per_chan_vis = 3;

static jack_client_t* client = NULL;
static jack_port_t* left = NULL;
static jack_port_t* right = NULL;
static unsigned int srate = 0;
static jack_nframes_t latency = 0;
static char* cleft = NULL;
static char* cright = NULL;
static bool want_autoconnect = true;
static char* wanted_client_name = NULL;
static bool want_transport = true;

static pthread_mutex_t xmpctx_lock;
static xmp_context xmpctx = NULL;
static struct xmp_module_info xmpminfo;
static struct xmp_frame_info xmpfinfo;
static size_t buffer_used = 0;
static bool new_frame = true;
static size_t num_channels = 0;
static size_t notif_len = 0;

static bool paused = false;
static bool want_shuffle = false;
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

static inline void expect_next_argument(int argc, char** argv, int i) {
	if(i == argc - 1) {
		fprintf(stderr, "Expected another argument after: %s\n", argv[i]);
		exit(1);
	}
}

static inline void transport_update(void) {
	if(!want_transport) return;
	if(paused) jack_transport_stop(client);
	else jack_transport_start(client);
}

static void restore_term(void) {
	printf("%c[?25h", 27); /* Show cursor */
	tcsetattr(0, TCSANOW, &pflags);
}

static int get_command(void) {
	static fd_set f;
	static struct timeval t;

	/* select() on stdin for reading, do not block */
	FD_ZERO(&f);
	FD_SET(fileno(stdin), &f);
	t.tv_sec = 0;
	t.tv_usec = 0;

	if(select(1, &f, NULL, NULL, &t) > 0) {
		char buf[64];
		ssize_t nbytes = read(fileno(stdin), buf, 64);

		if(nbytes == 1) return buf[0];
		if(nbytes == 2) return (buf[0] << 8) | buf[1];
		if(nbytes == 3) return (buf[0] << 16) | (buf[1] << 8) | buf[2];
	}

	return 0;
}

static int jack_process(jack_nframes_t nframes, void* unused) {
	float* lbuf = jack_port_get_buffer(left, nframes);
	float* rbuf = jack_port_get_buffer(right, nframes);

	if(want_transport) {
		jack_position_t pos;
		jack_transport_state_t state = jack_transport_query(client, &pos);

		paused = (state != JackTransportRolling);
		/* XXX: handle timecode? */
	}

	if(paused || pthread_mutex_trylock(&xmpctx_lock)) {
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
			goto end;
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

 end:
	pthread_mutex_unlock(&xmpctx_lock);
	return 0;
}

static void jack_latency(jack_latency_callback_mode_t mode, void* unused) {
	if(mode != JackPlaybackLatency) return;
	if(client == NULL || left == NULL) return;

	jack_latency_range_t range;
	jack_port_get_latency_range(left, mode, &range);
	if(latency == range.max || range.max == 0) return;

	printf("\rJACK: playback latency is %d~%d frames (%.2f~%.2f ms)" EOL,
		   range.min, range.max,
		   1000.f * range.min / srate, 1000.f * range.max / srate);
	latency = range.max;
}

static void jack_timebase(jack_transport_state_t state, jack_nframes_t nframes, jack_position_t* pos, int new_pos, void* unused) {
	pos->valid = JackPositionBBT | JackPositionTimecode | JackBBTFrameOffset;

	pos->bar = 1 + xmpfinfo.pos;
	pos->beats_per_minute = xmpfinfo.bpm;

	/* XXX */
	/* tick duration (ms) = 2500 / bpm */
	/* beat duration (ms) = 60000 / bpm */

	/* elapsed = (row * speed + frame) * 2500 / bpm */
	/* beat = (row * speed + frame) * 2500 / bpm / (60000 / bpm) */

	pos->beat_type = 1.f;
	pos->beats_per_bar = (xmpfinfo.num_rows * xmpfinfo.speed) / 24.f;
	pos->beat = 1 + (xmpfinfo.row * xmpfinfo.speed + xmpfinfo.frame) / 24.f;

	pos->ticks_per_beat = 24;
	pos->bar_start_tick = xmpfinfo.frame + xmpfinfo.speed * xmpfinfo.row;
	pos->tick = (xmpfinfo.frame + xmpfinfo.speed * xmpfinfo.row) % 24;

	pos->frame_time = (double)xmpfinfo.time / 1000.0 + xmpfinfo.loop_count * (double)xmpfinfo.total_time / 1000.0;
	pos->next_time = pos->frame_time + (double)xmpfinfo.frame_time / 1000000.0;
	pos->bbt_offset = 0;
	pos->frame = pos->frame_time * pos->frame_rate;
}

static void usage(FILE* to, char* me) {
	fprintf(
		to,
		"\rUsage: %s [options] [--] <modfiles...>" EOL
		"\n"
		"Options:\n"
		"\t--jack-client-name foo\n"
		"\t\tUse custom JACK client name (default xmpjack)\n"
		"\t-n, --jack-no-autoconnect\n"
		"\t\tDo not autoconnect to first available physical ports\n"
		"\t--jack-no-transport\n"
		"\t\tDo not rely on JACK transport for play/pause/seek\n"
		"\t--jack-connect-left foo, --jack-connect-right bar\n"
		"\t\tConnect to specified JACK ports before playback\n"
		"\t-l, --loop\n"
		"\t\tEnable looping of modules (default is no looping)\n"
		"\t-p, --paused\n"
		"\t\tDon't automatically start playback\n"
		"\t-s, --shuffle\n"
		"\t\tPlay back modules in random order\n"
		"\n"
		"Interactive commands:\n"
		"\tq\tQuit the program\n"
		"\tSPC\tToggle play/pause\n"
		"\tn\tPlay next module\n"
		"\tp\tPlay previous module\n"
		"\t/*\tIncrease/decrease gain by 1 dB\n"
		"\tUp/Dn\tPattern seeking\n"
		"\n"
		, me);
}

static void print_notif(const char *fmt, ...) {
	va_list ap;

	notif_until = jack_get_time() + 1000000;

	putchar('\r');
	va_start(ap, fmt);
	notif_len = vprintf(fmt, ap);
	va_end(ap);
	printf("\e[0K");

	fflush(stdout);
}

static void shuffle_array(void** arr, size_t len) {
	/* Knuth array shuffle */

	size_t r;
	void* e;

	srand(jack_get_time());

	for(size_t i = len - 1; i > 0; --i) {
		r = rand() % (i + 1);
		e = arr[i];
		arr[i] = arr[r];
		arr[r] = e;
	}
}

static void print_vis(void) {
	if(jack_get_time() < notif_until) return;

	for(size_t j = XMP_MAX_CHANNELS - 1; j > 0; --j) {
		struct xmp_channel_info* info = &xmpfinfo.channel_info[j];
		if(info->period == 0 || info->volume == 0) continue;
		num_channels = j + 1;
		break;
	}

	putchar('\r');
	for(size_t j = 0; j < num_channels; ++j) {
		struct xmp_channel_info* info = &xmpfinfo.channel_info[j];

		if(info->period == 0 || info->volume == 0) {
			printf("%*c", per_chan_vis, ' ');
		} else {
			float foctave = log2f(1.f / info->period);
			int octave = (int)floorf(foctave);
			int note = ((int)roundf((foctave - octave) * 12.f) + 9) % 12;
			float vol = (float)info->volume / xmpminfo.vol_base;

			octave += 25;
			if(octave > 9) octave = 9;
			else if(octave < 0) octave = 0;

			printf("%c[%d%sm%s%01d%c[0m",
				   27,
				   31 + (info->instrument % 6),
				   (vol >= .66) ? ";1" : "",
				   (vol >= .33) ? Notes[note] : notes[note],
				   octave,
				   27);
		}
	}
	printf("\e[0K");
	fflush(stdout);
}

static int jack_xrun(void* unused) {
	printf("\rJACK: xrun :-(" EOL);
	return 0;
}

static int parse_args(int argc, char** argv) {
	size_t j = 1;

	for(size_t i = 1; i < argc; ++i) {
		char* arg = argv[i];
		if(arg[0] != '-') {
			/* Reached positional arguments */
			return i;
		}

		/* Stop parsing arguments after -- */
		if(arg[0] == '-' && arg[1] == '-' && arg[2] == '\0') {
			return i+1;
		}

		/* XXX: I know this sucks hard, if you find a better way
		 * please tell me about it (other than using getopt) */

		if(arg[1] != '-') {
			/* Parsing short options */
			if(arg[j]) {
				--i;
				++j;

				switch(arg[j - 1]) {
				case 'l': goto toggle_loop;
				case 'p': goto toggle_pause;
				case 's': goto toggle_shuffle;
				case 'n': goto toggle_autoconnect;

				default:
					fprintf(stderr, "Unknown option: -%c\n", arg[j]);
					exit(1);
				}
			} else {
				continue;
			}
		} else if(arg[1] == '-') {
			/* Parsing long option */
			char* name = &arg[2];
			j = 1;

			if(!strcmp("loop", name)) goto toggle_loop;
			if(!strcmp("paused", name)) goto toggle_pause;
			if(!strcmp("shuffle", name)) goto toggle_shuffle;
			if(!strcmp("jack-connect-left", name)) goto connect_left;
			if(!strcmp("jack-connect-right", name)) goto connect_right;
			if(!strcmp("jack-no-autoconnect", name)) goto toggle_autoconnect;
			if(!strcmp("jack-client-name", name)) goto jack_client_name;
			if(!strcmp("jack-no-transport", name)) goto toggle_jack_transport;

			fprintf(stderr, "Unknown long option: %s\n", arg);
			exit(1);
		}

		assert(false);

	toggle_loop:
		loop = !loop;
		continue;

	toggle_pause:
		paused = !paused;
		continue;

	toggle_shuffle:
		want_shuffle = !want_shuffle;
		continue;

	connect_left:
		expect_next_argument(argc, argv, i);
		cleft = argv[++i];
		continue;

	connect_right:
		expect_next_argument(argc, argv, i);
		cright = argv[++i];
		continue;

	toggle_autoconnect:
		want_autoconnect = !want_autoconnect;
		continue;

	jack_client_name:
		expect_next_argument(argc, argv, i);
		wanted_client_name = argv[++i];
		continue;

	toggle_jack_transport:
		want_transport = !want_transport;
		continue;

	}

	return argc;
}

int main(int argc, char** argv) {
	if(argc == 1) {
		usage(stderr, argv[0]);
		exit(1);
	}

	int i0 = parse_args(argc, argv);

	atexit(restore_term);
	tcgetattr(0, &pflags);
	cflags = pflags;
	cflags.c_lflag &= ~ECHO;
	cflags.c_lflag &= ~ICANON;
	cflags.c_lflag |= ECHONL;
	tcsetattr(0, TCSANOW, &cflags);
	printf("%c[?25l", 27); /* Hide cursor */

	client = jack_client_open(wanted_client_name == NULL ? "xmpjack" : wanted_client_name, JackNullOption, NULL);
	if(client == NULL) return 1;

	const char* client_name = jack_get_client_name(client);
	char lport_name[strlen(client_name) + 6];
	char rport_name[strlen(client_name) + 7];
	sprintf(lport_name, "%s:Left", client_name);
	sprintf(rport_name, "%s:Right", client_name);

	printf("JACK: client name is %s\n", client_name);
	printf("JACK: buffer size is %d frames\n", jack_get_buffer_size(client)); /* XXX: use callback */
	printf("JACK: sample rate is %d Hz\n", srate = jack_get_sample_rate(client)); /* XXX: use callback (hard) */

	jack_set_process_callback(client, jack_process, NULL);
	jack_set_latency_callback(client, jack_latency, NULL);
	jack_set_xrun_callback(client, jack_xrun, NULL);

	if(want_transport) {
		/* Be a transport master only if there is none currently */
		if(!jack_set_timebase_callback(client, 1, jack_timebase, NULL)) {
			printf("JACK: became timebase master\n");
		}
	}

	/* Default jack format: signed float */
	left = jack_port_register(client, "Left", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
	right = jack_port_register(client, "Right", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);

	printf("Creating xmp context, libxmp version %s.\n", xmp_version);
	pthread_mutex_init(&xmpctx_lock, 0);
	pthread_mutex_lock(&xmpctx_lock);
	xmpctx = xmp_create_context();

	if(want_shuffle) {
		shuffle_array((void**)(&argv[i0]), argc - i0);
	}

	jack_activate(client);
	transport_update();

	if(want_autoconnect) {
		const char** ports = jack_get_ports(client, NULL, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal | JackPortIsPhysical);
		if(ports[0] == NULL) {
			printf("JACK: no autoconnect candidates\n");
		} else if(ports[1] == NULL) {
			/* Mono setup? Should be rare */
			cleft = cright = strdup(ports[0]);
		} else {
			cleft = strdup(ports[0]);
			cright = strdup(ports[1]);
		}
		jack_free(ports);
	}
	if(cleft != NULL) jack_connect(client, lport_name, cleft);
	if(cright != NULL) jack_connect(client, rport_name, cright);

	for(int i = i0; i < argc; ++i) {
		printf("\rLoading %s..." EOL, argv[i]);
		fflush(stdout);
		if(xmp_load_module(xmpctx, argv[i]) != 0) {
			fprintf(stderr, "\rModule %s could not be loaded by libxmp.\n", argv[i]);
			continue;
		}
		xmp_get_module_info(xmpctx, &xmpminfo);
		printf("\rPlaying back %s.\n", argv[i]);

		/* Default xmp sample format: s16 stereo interleaved */
		xmp_start_player(xmpctx, srate, 0);
		render_frame();
		prev_loop_count = 0;

		/* XXX: make these user tuneable */
		xmp_set_player(xmpctx, XMP_PLAYER_AMP, 0);
		xmp_set_player(xmpctx, XMP_PLAYER_MIX, 100);
		xmp_set_player(xmpctx, XMP_PLAYER_INTERP, XMP_INTERP_NEAREST);
		pthread_mutex_unlock(&xmpctx_lock);

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
				transport_update();
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
				if(i < i0) i = i0 - 1;
				goto end;
				break;

			case 'h':
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

			case 0x1b5b41: /* Up */
			case 0x1b5b43: /* Right */
				print_notif("Next pattern in POT [%02X/%02X]", (xmpfinfo.pos + 1) & 0xff, xmpminfo.mod->len);
				xmp_next_position(xmpctx);
				break;

			case 0x1b5b42: /* Down */
			case 0x1b5b44: /* Left */
				print_notif("Previous pattern in POT [%02X/%02X]", (xmpfinfo.pos - 1) & 0xff, xmpminfo.mod->len);
				xmp_prev_position(xmpctx);
				break;

			default:
			case 0:
				break;
			}

			usleep(10000);
		}

	end:
		pthread_mutex_lock(&xmpctx_lock);
		xmp_end_player(xmpctx);

		/* XXX: fixes "lingering channels when navigating"
		 * bug. there's probably a way to avoid this altogether by
		 * querying number of used channels? */
		memset(&xmpfinfo, 0, sizeof(struct xmp_frame_info));
	}

	xmp_free_context(xmpctx);
	jack_client_close(client);
	pthread_mutex_destroy(&xmpctx_lock);
	printf("\rExiting." EOL);
	return 0;
}
