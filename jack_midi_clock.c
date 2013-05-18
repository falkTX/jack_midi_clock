/* JACK-Transport MIDI Beat Clock Generator
 *
 * Copyright (C) 2013 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2009 Gabriel M. Beddingfield <gabriel@teuton.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <getopt.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#include <sys/mman.h>

#ifndef WIN32
#include <signal.h>
#endif

/* bitwise flags -- used w/ msg_filter */
enum {
  MSG_NO_TRANSPORT  = 1,
  MSG_NO_CLOCK      = 2,
  MSG_NO_POSITION   = 4,
  MSG_NO_CONT_CLOCK = 8
};

/* jack_position_t excerpt */
struct bbtpos {
  jack_position_bits_t valid;  /**< which other fields are valid */
  int32_t      bar;            /**< current bar */
  int32_t      beat;           /**< current beat-within-bar */
  int32_t      tick;           /**< current tick-within-beat */
  jack_double  bar_start_tick; /**< number of ticks that have elapsed between frame 0 and the first beat of the current measure. */
};

/* jack connection */
static jack_port_t            *mclk_output_port = NULL;
static jack_client_t          *j_client = NULL;

/* application state */
static jack_transport_state_t  m_xstate = JackTransportStopped;
static double                  mclk_last_tick = 0.0;

static volatile enum {
  Init,
  Run,
  Exit
} client_state = Init;

static struct bbtpos last_xpos;

/* commandline options */
static double                  user_bpm   = 0.0;
static short                   force_bpm  = 0;
static short                   msg_filter = MSG_NO_CONT_CLOCK;

/* MIDI System Real-Time Messages
 * https://en.wikipedia.org/wiki/MIDI_beat_clock
 * http://www.midi.org/techspecs/midimessages.php
 */
#define MIDI_RT_CLOCK    (0xF8)
#define MIDI_RT_START    (0xFA)
#define MIDI_RT_CONTINUE (0xFB)
#define MIDI_RT_STOP     (0xFC)

/**
 * cleanup and exit
 * call this function only _after_ everything has been initialized!
 */
static void cleanup(int sig) {
  if (j_client) {
    jack_client_close (j_client);
    j_client = NULL;
  }
  fprintf(stderr, "bye.\n");
}

/**
 * send 1 byte MIDI Message
 * @param port_buf buffer to write event to
 * @param time sample offset of event
 * @param rt_msg message byte
 */
static void send_rt_message(void* port_buf, jack_nframes_t time, uint8_t rt_msg) {
  uint8_t *buffer;
  buffer = jack_midi_event_reserve(port_buf, time, 1);
  if(buffer) {
    buffer[0] = rt_msg;
  }
}

static void send_pos_message(void* port_buf, jack_position_t *xpos) {
  uint8_t *buffer;
  if (msg_filter & MSG_NO_POSITION) return;
  if (!(xpos->valid & JackPositionBBT)) return;

  /* send '0xf2' Song Position Pointer.
   * This is an internal 14 bit register that holds the number of
   * MIDI beats (1 beat = six MIDI clocks) since the start of the song.
   * l is the LSB, m the MSB
   *
   *  MIDI Beat Clock: 24 ticks per quarter note
   *  one MIDI-beat = six MIDI clocks
   *  -> 4 MIDI-beats per quarter note (jack beat)
   *  Note: jack counts bars and beats starting at 1
   */
  const int bcnt =
      4 * ((xpos->bar - 1) * xpos->beats_per_bar + (xpos->beat - 1))
    + floor(4.0 * xpos->tick / (double) xpos->ticks_per_beat);

  if (bcnt < 0 || bcnt >= 16384) {
    return;
  }
  buffer = jack_midi_event_reserve(port_buf, 0, 3);
  if(!buffer) {
    return;
  }
  buffer[0] = 0xf2;
  buffer[1] = (bcnt)&0x7f;  // LSB
  buffer[2] = (bcnt>>7)&0x7f;  // MSB
}

static int pos_changed (struct bbtpos *xp0, jack_position_t *xp1) {
  if (!(xp0->valid & JackPositionBBT)) return -1;
  if (!(xp1->valid & JackPositionBBT)) return -2;
  if (   xp0->bar  == xp1->bar
      && xp0->beat == xp1->beat
      && xp0->tick == xp1->tick
     ) return 0;
  return 1;
}

static void remember_pos (struct bbtpos *xp0, jack_position_t *xp1) {
  if (!(xp1->valid & JackPositionBBT)) return;
  xp0->valid = xp1->valid;
  xp0->bar   = xp1->bar;
  xp0->beat  = xp1->beat;
  xp0->tick  = xp1->tick;
  xp0->bar_start_tick = xp1->bar_start_tick;
}

/**
 * jack process callback
 */
static int process (jack_nframes_t nframes, void *arg) {
  double samples_per_beat;
  jack_nframes_t bbt_offset = 0;
  jack_position_t xpos;
  jack_transport_state_t xstate = jack_transport_query(j_client, &xpos);
  void* port_buf = jack_port_get_buffer(mclk_output_port, nframes);
  jack_midi_clear_buffer(port_buf);

  if (client_state != Run) {
    return 0;
  }

  /* send position updates if stopped and located */
  if (xstate == JackTransportStopped && xstate == m_xstate) {
    if (pos_changed(&last_xpos, &xpos) > 0) {
      send_pos_message(port_buf, &xpos);
    }
  }
  remember_pos(&last_xpos, &xpos);

  /* send RT messages start/stop/continue */
  if( xstate != m_xstate ) {
    switch(xstate) {
      case JackTransportStopped:
	if (!(msg_filter & MSG_NO_TRANSPORT)) {
	  send_rt_message(port_buf, 0, MIDI_RT_STOP);
	}
	send_pos_message(port_buf, &xpos);
	break;
      case JackTransportRolling:
	if(m_xstate == JackTransportStarting) {
	  /*
	   * TODO send stop
	   * send_pos_message() ~ 3-4 second in the future
	   * keep sending clocks
	   * send 'start/continue' at the same time with clock signal
	   * when transport reaches the specified position.
	   */
	  //send_pos_message(port_buf, &xpos); // XXX
	}
      case JackTransportStarting:
	if(m_xstate == JackTransportStarting) {
	  break;
	}
	if( xpos.frame == 0 ) {
	  if (!(msg_filter & MSG_NO_TRANSPORT)) {
	    send_rt_message(port_buf, 0, MIDI_RT_START);
	  }
	} else {
	  if (!(msg_filter & MSG_NO_TRANSPORT)) {
	    send_rt_message(port_buf, 0, MIDI_RT_CONTINUE);
	  }
	}
	break;
      default:
	break;
    }

    /* initial beat tick */
    if (xstate == JackTransportRolling) {
      if (!(msg_filter & MSG_NO_CLOCK)) {
	send_rt_message(port_buf, 0, MIDI_RT_CLOCK);
      }
    }

    mclk_last_tick = xpos.frame;
    m_xstate = xstate;
  }

  if((xstate != JackTransportRolling) && (msg_filter & MSG_NO_CONT_CLOCK)) {
    return 0;
  }

  if (msg_filter & MSG_NO_CLOCK) {
    /* TODO allow to switch dynamically (SIGUSR1)?
     * -> keep counting mclk_last_tick
     */
    return 0;
  }

  /* calculate clock tick interval */
  if(force_bpm && user_bpm > 0) {
    samples_per_beat = (double) xpos.frame_rate * 60.0 / user_bpm;
  }
  else if(xpos.valid & JackPositionBBT) {
    samples_per_beat = (double) xpos.frame_rate * 60.0 / xpos.beats_per_minute;
    if (xpos.valid & JackBBTFrameOffset) {
      bbt_offset = xpos.bbt_offset;
    }
  }
  else if(user_bpm > 0) {
    samples_per_beat = (double) xpos.frame_rate * 60.0 / user_bpm;
  } else {
    return 0;
  }

  /* the quarter-notes per beat is usually independent of meter, isn't it?!
   * it's true for 2/4, 3/4, 4/4 etc.
   * should be true as well for 6/8, 2/2 -- x-check w/timecode masters
   *
   * quarter_notes_per_beat = xpos.beat_type / 4.0;
   */
  const double quarter_notes_per_beat = 1.0;

  /* MIDI Beat Clock: Send 24 ticks per quarter note  */
  const double samples_per_quarter_note = samples_per_beat / quarter_notes_per_beat;
  const double clock_tick_interval = samples_per_quarter_note / 24.0;

  /* send clock ticks for this cycle */
  while(1) {
    const double next_tick = mclk_last_tick + clock_tick_interval;
    const int64_t next_tick_offset = llrint(next_tick) - xpos.frame - bbt_offset;
    if (next_tick_offset >= nframes) break;
    if (next_tick_offset >= 0) {
      send_rt_message(port_buf, next_tick_offset, MIDI_RT_CLOCK);
    }
    mclk_last_tick = next_tick;
  }
  return 0;
}

/**
 * callback if jack server terminates
 */
static void jack_shutdown (void *arg) {
  fprintf(stderr, "recv. shutdown request from jackd.\n");
  client_state = Exit;
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

  return (0);
}

static int jack_portsetup(void) {
  if ((mclk_output_port = jack_port_register(j_client, "mclk_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0)) == 0) {
    fprintf (stderr, "cannot register mclk ouput port !\n");
    return (-1);
  }
  return (0);
}

static void port_connect(char *mclk_port) {
  if (mclk_port && jack_connect(j_client, jack_port_name(mclk_output_port), mclk_port)) {
    fprintf(stderr, "cannot connect port %s to %s\n", jack_port_name(mclk_output_port), mclk_port);
  }
}

static void catchsig (int sig) {
#ifndef _WIN32
  signal(SIGHUP, catchsig);
#endif
  fprintf(stderr,"caught signal - shutting down.\n");
  client_state = Exit;
}

/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"bpm", required_argument, 0, 'b'},
  {"force-bpm", no_argument, 0, 'B'},
  {"help", no_argument, 0, 'h'},
  {"no-position", no_argument, 0, 'P'},
  {"no-transport", no_argument, 0, 'T'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jack_midi_clock - JACK app to generate MCLK from JACK transport.\n\n");
  printf ("Usage: jack_midi_clock [ OPTIONS ] [JACK-port]*\n\n");
  printf ("Options:\n"

"  -b, --bpm <num>        default BPM (if jack timecode master in not available)\n"
"  -B, --force-bpm        ignore jack timecode master\n"
"  -P, --no-position      do not send song-position (0xf2) messages\n"
"  -T, --no-transport     do not send start/stop/continue messages\n"
"  -h, --help             display this help and exit\n"
"  -V, --version          print version information and exit\n"

"\n");
  printf ("\n"

/*                                  longest help text w/80 chars per line ---->|\n" */

"jack_midi_clock will send start, continue and stop messages whenever\n"
"the transport changes state.\n"
"\n"
"In order for jack_midi_clock to send clock messages, a JACK timecode master\n"
"must be present and provide the tempo map (bar, beat, tick).\n"
"Alternatively the -b option can be used to set a default BPM value.\n"
"If a value larger than zero is given, it will be used if no timecode master\n"
"is present. Combined with the -B option it can used to override and ignore\n"
"JACK timecode master.\n"
"\n"
"Either way, jack_midi_clock will never act as timecode master itself.\n"
"\n"
"Note that song-position information is only sent if a timecode master\n"
"is present.\n"
"\n"
"jack_midi_clock runs until it receives a HUP or INT signal or jackd is\n"
"terminated.\n"
"\n"
"See also: jack_transport(1), jack_mclk_dump(1)\n"

"\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
          "Website: https://github.com/x42/jack_midi_clock/\n"
	  );
  exit (status);
}

static int decode_switches (int argc, char **argv) {
  int c;

  while ((c = getopt_long (argc, argv,
			   "b:"	/* bpm */
			   "B"	/* force-bpm */
			   "h"	/* help */
			   "P"	/* no-position */
			   "T"	/* no-transport */
			   "V",	/* version */
			   long_options, (int *) 0)) != EOF)
    {
      switch (c) {
	case 'b':
	  user_bpm = atof(optarg);
	  break;

	case 'B':
	  force_bpm = 1;
	  break;

	case 'P':
	  msg_filter |= MSG_NO_POSITION;
	  break;

	case 'T':
	  msg_filter |= MSG_NO_TRANSPORT;
	  break;

	case 'V':
	  printf ("jack_midi_clock version %s\n\n", VERSION);
	  printf ("Copyright (C) GPL 2013 Robin Gareus <robin@gareus.org>\n");
	  printf ("Copyright (C) GPL 2009 Gabriel M. Beddingfield <gabriel@teuton.org>\n");
	  exit (0);

	case 'h':
	  usage (0);

	default:
	  usage (EXIT_FAILURE);
      }
    }

  return optind;
}

int main (int argc, char **argv) {

  decode_switches (argc, argv);

  if (init_jack("jack_midi_clock"))
    goto out;
  if (jack_portsetup())
    goto out;

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
  signal (SIGHUP, catchsig);
  signal (SIGINT, catchsig);
#endif

  memset(&last_xpos, 0, sizeof(struct bbtpos));

  client_state = Run;
  while (client_state != Exit) {
    sleep (1);
  }

out:
  cleanup(0);
  return(0);
}

/* vi:set ts=8 sts=2 sw=2: */
