/* JACK MIDI Beat Clock Parser
 *
 * (C) 2013  Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#ifdef WIN32
#include <windows.h>
#include <pthread.h>
#define pthread_t //< override jack.h def
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <math.h>
#include <sys/mman.h>

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#endif

#include <jack/jack.h>
#include <jack/transport.h>
#include <jack/ringbuffer.h>
#include <jack/midiport.h>

#define RBSIZE 20

typedef struct {
	uint8_t msg;
	unsigned long long int tme;
} timenfo;

static jack_ringbuffer_t *rb = NULL;
static pthread_mutex_t msg_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_ready = PTHREAD_COND_INITIALIZER;

/* options */
char newline = '\r'; // or '\n';


/************************************************
 * jack-midi
 */

jack_client_t *j_client = NULL;
jack_port_t   *mclk_input_port;

static uint32_t j_samplerate = 48000;
static volatile unsigned long long monotonic_cnt = 0;

static void process_jmidi_event(jack_midi_event_t *ev, unsigned long long mfcnt) {
	timenfo tnfo;
	memset(&tnfo, 0, sizeof(timenfo));
	if (ev->size==1 && ev->buffer[0] == 0xf8) {
		tnfo.msg = 0xf8;
		tnfo.tme = mfcnt + ev->time;

		if (jack_ringbuffer_write_space(rb) >= sizeof(timenfo)) {
			jack_ringbuffer_write(rb, (void *) &tnfo, sizeof(timenfo));
		}

		if (pthread_mutex_trylock (&msg_thread_lock) == 0) {
			pthread_cond_signal (&data_ready);
			pthread_mutex_unlock (&msg_thread_lock);
		}
	}
}

static int process(jack_nframes_t nframes, void *arg) {
	void *jack_buf = jack_port_get_buffer(mclk_input_port, nframes);
	int nevents = jack_midi_get_event_count(jack_buf);
	int n;

	for (n=0; n<nevents; n++) {
		jack_midi_event_t ev;
		jack_midi_event_get(&ev, jack_buf, n);
		process_jmidi_event(&ev, monotonic_cnt);
	}
	monotonic_cnt += nframes;
	return 0;
}

void jack_shutdown(void *arg) {
	j_client=NULL;
	pthread_cond_signal (&data_ready);
	fprintf (stderr, "jack server shutdown\n");
}

void cleanup(void) {
	if (j_client) {
		jack_deactivate (j_client);
		jack_client_close (j_client);
	}
	if (rb) {
		jack_ringbuffer_free(rb);
	}
	j_client = NULL;
}

/**
 * open a client connection to the JACK server
 */
static int init_jack(const char *client_name) {
	jack_status_t status;
	j_client = jack_client_open (client_name, JackNullOption, &status);
	if (j_client == NULL) {
		fprintf (stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
		if (status & JackServerFailed) {
			fprintf (stderr, "Unable to connect to JACK server\n");
		}
		return (-1);
	}
	if (status & JackServerStarted) {
		fprintf (stderr, "JACK server started\n");
	}
	if (status & JackNameNotUnique) {
		client_name = jack_get_client_name(j_client);
		fprintf (stderr, "jack-client name: `%s'\n", client_name);
	}
	jack_set_process_callback (j_client, process, 0);

#ifndef WIN32
	jack_on_shutdown (j_client, jack_shutdown, NULL);
#endif
	j_samplerate=jack_get_sample_rate (j_client);

	return (0);
}

static int jack_portsetup(void) {
	if ((mclk_input_port = jack_port_register(j_client, "mclk_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0)) == 0) {
		fprintf (stderr, "cannot register mclk input port !\n");
		return (-1);
	}
	return (0);
}

static void port_connect(char *mclk_port) {
	if (mclk_port && jack_connect(j_client, mclk_port, jack_port_name(mclk_input_port))) {
		fprintf(stderr, "cannot connect port %s to %s\n", mclk_port, jack_port_name(mclk_input_port));
	}
}


/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"help", no_argument, 0, 'h'},
  {"newline", no_argument, 0, 'n'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jack_mclk_dump - JACK MIDI Clock dump.\n\n");
  printf ("Usage: jack_mclk_dump [ OPTIONS ] [JACK-port]\n\n");
  printf ("Options:\n\
  -h, --help                 display this help and exit\n\
  -n, --newline              print a newline after each Tick\n\
  -V, --version              print version information and exit\n\
\n");
  printf ("\n\
This tool subscribes to a JACK Midi Port and prints received Midi\n\
beat clock and BPM to stdout.\n\
\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
          "Website and manual: <https://github.com/x42/jack_midi_clock>\n"
	  );
  exit (status);
}

static int decode_switches (int argc, char **argv) {
	int c;

	while ((c = getopt_long (argc, argv,
			   "h"	/* help */
			   "n"	/* newline */
			   "V",	/* version */
			   long_options, (int *) 0)) != EOF) {
		switch (c) {
			case 'n':
				newline = '\n';
				break;
			case 'V':
				printf ("jack_mclk_dump version %s\n\n", VERSION);
				printf ("Copyright (C) GPL 2013 Robin Gareus <robin@gareus.org>\n");
				exit (0);

			case 'h':
				usage (0);

			default:
			  usage (EXIT_FAILURE);
		}
	}
	return optind;
}

static int run = 1;

void wearedone(int sig) {
	fprintf(stderr,"caught signal - shutting down.\n");
	run=0;
	pthread_cond_signal (&data_ready);
}

int main (int argc, char ** argv) {
	int i;
	timenfo pt;

	decode_switches (argc, argv);

	if (init_jack("jack_mclk_dump"))
		goto out;
	if (jack_portsetup())
		goto out;

	rb = jack_ringbuffer_create(RBSIZE * sizeof(timenfo));

	if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
		fprintf(stderr, "Warning: Can not lock memory.\n");
	}

	if (jack_activate (j_client)) {
		fprintf (stderr, "cannot activate client.\n");
		goto out;
	}

	while (optind < argc)
		port_connect(argv[optind++]);

#ifndef _WIN32
	signal(SIGINT, wearedone);
#endif

	memset(&pt, 0, sizeof(timenfo));
	pthread_mutex_lock (&msg_thread_lock);

	while (run && j_client) {
		const int mqlen = jack_ringbuffer_read_space (rb) / sizeof(timenfo);
		for (i=0; i < mqlen; ++i) {
			timenfo t;
			jack_ringbuffer_read(rb, (char*) &t, sizeof(timenfo));
			// TODO print start/stop/continue msg
			// DLL, reset DLL on start/stop/cont...
			if (t.msg == 0xf8) {
				const double samples_per_quarter_note = (t.tme - pt.tme) * 24.0;
				const double bpm = j_samplerate * 60.0 / samples_per_quarter_note;
				fprintf(stdout, "%.2f @ %lld%c", bpm, t.tme, newline);
			}
			memcpy(&pt, &t, sizeof(timenfo));
			fflush(stdout);
		}
		pthread_cond_wait (&data_ready, &msg_thread_lock);
	}
	pthread_mutex_unlock (&msg_thread_lock);

out:
	cleanup();
	return 0;
}
