#include <stdio.h>
#include <errno.h>
#include <unistd.h>

extern "C"
{
#include <jack/jack.h>
}

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Slider.H>

jack_port_t *my_input_port;
jack_port_t *my_output_port;

float gain = 0.0; /* slider starts out with zero gain */

int
process (nframes_t nframes, void *arg)

{
	sample_t *out = (sample_t *) jack_port_get_buffer (my_output_port, nframes);
	sample_t *in = (sample_t *) jack_port_get_buffer (my_input_port, nframes);

	while (nframes--)
		out[nframes] = in[nframes] * gain;

	return 0;      
}

int
bufsize (nframes_t nframes, void *arg)

{
	printf ("the maximum buffer size is now %lu\n", nframes);
	return 0;
}

int
srate (nframes_t nframes, void *arg)

{
	printf ("the sample rate is now %lu/sec\n", nframes);
	return 0;
}

void callback(Fl_Slider* s)
{
	gain = s->value();
}

int
main (int argc, char *argv[])

{
	Fl_Window w(0,0,100,120);
	Fl_Slider s(10,10,20,100);
	w.show();
	s.callback((Fl_Callback*) callback);
	
	jack_client_t *client;

	if ((client = jack_client_new ("fltktest")) == 0) {
		fprintf (stderr, "jack server not running?\n");
		return 1;
	}

	jack_set_process_callback (client, process, 0);
	jack_set_buffer_size_callback (client, bufsize, 0);
	jack_set_sample_rate_callback (client, srate, 0);

	printf ("engine sample rate: %lu\n", jack_get_sample_rate (client));

	my_input_port = jack_port_register (client, "myinput", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	my_output_port = jack_port_register (client, "myoutput", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

	if (jack_activate (client)) {
		fprintf (stderr, "cannot activate client");
	}

	printf ("client activated\n");

	if (jack_port_connect (client, "ALSA I/O:Input 1", my_input_port->shared->name)) {
		fprintf (stderr, "cannot connect input ports\n");
	} 
	
	if (jack_port_connect (client, my_output_port->shared->name, "ALSA I/O:Output 1")) {
		fprintf (stderr, "cannot connect output ports\n");
	} 

	Fl::run();

	printf ("done sleeping, now closing...\n");
	jack_client_close (client);
	exit (0);
}
