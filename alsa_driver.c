/*
    Copyright (C) 2001 Paul Davis 

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id$
*/

#include <math.h>
#include <stdio.h>
#include <memory.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <asm/msr.h>
#include <glib.h>
#include <stdarg.h>

#include <jack/alsa_driver.h>
#include <jack/types.h>
#include <jack/internal.h>
#include <jack/engine.h>
#include <jack/hammerfall.h>
#include <jack/generic.h>

static int  config_max_level = 0;
static int  config_min_level = 0;

static unsigned long current_usecs () {
	unsigned long now;
	rdtscl (now);
	return now / 450;
}

static void
alsa_driver_release_channel_dependent_memory (alsa_driver_t *driver)

{
	if (driver->playback_addr) {
		free (driver->playback_addr);
		driver->playback_addr = 0;
	}

	if (driver->capture_addr) {
		free (driver->capture_addr);
		driver->capture_addr = 0;
	}

	if (driver->silent) {
		free (driver->silent);
		driver->silent = 0;
	}

	if (driver->input_monitor_requests) {
		free (driver->input_monitor_requests);
		driver->input_monitor_requests = 0;
	}
}

static int
alsa_driver_check_capabilities (alsa_driver_t *driver)

{
	return 0;
}

static int
alsa_driver_check_card_type (alsa_driver_t *driver)

{
	int err;
	snd_ctl_card_info_t *card_info;

	snd_ctl_card_info_alloca (&card_info);

	if ((err = snd_ctl_open (&driver->ctl_handle, driver->alsa_name, 0)) < 0) {
		jack_error ("control open \"%s\" (%s)", driver->alsa_name, snd_strerror(err));
		return -1;
	}
	
	if ((err = snd_ctl_card_info(driver->ctl_handle, card_info)) < 0) {
		jack_error ("control hardware info \"%s\" (%s)", driver->alsa_name, snd_strerror (err));
		snd_ctl_close (driver->ctl_handle);
		return -1;
	}

	driver->alsa_driver = strdup(snd_ctl_card_info_get_driver (card_info));

	return alsa_driver_check_capabilities (driver);
}

static int
alsa_driver_hammerfall_hardware (alsa_driver_t *driver)

{
	driver->hw = jack_alsa_hammerfall_hw_new (driver);
	return 0;
}

static int
alsa_driver_generic_hardware (alsa_driver_t *driver)

{
	driver->hw = jack_alsa_generic_hw_new (driver);
	return 0;
}

static int
alsa_driver_hw_specific (alsa_driver_t *driver)

{
	int err;

	if (!strcmp(driver->alsa_driver, "RME9652")) {
		if ((err = alsa_driver_hammerfall_hardware (driver)) != 0) {
			return err;
		}
	} else {
		if ((err = alsa_driver_generic_hardware (driver)) != 0) {
			return err;
		}
	}

	if (driver->hw->capabilities & Cap_HardwareMonitoring) {
		driver->has_hw_monitoring = TRUE;
	} else {
		driver->has_hw_monitoring = FALSE;
	}
	
	/* XXX need to ensure that this is really FALSE */

	driver->hw_monitoring = FALSE;

	if (driver->hw->capabilities & Cap_ClockLockReporting) {
		driver->has_clock_sync_reporting = TRUE;
	} else {
		driver->has_clock_sync_reporting = FALSE;
	}

	return 0;
}

static void
alsa_driver_setup_io_function_pointers (alsa_driver_t *driver)

{
	switch (driver->sample_bytes) {
	case 2:
		if (driver->interleaved) {
			driver->channel_copy = memcpy_interleave_d16_s16;
		} else {
			driver->channel_copy = memcpy_fake;
		}
		
		driver->write_via_copy = sample_move_d16_sS;
		driver->read_via_copy = sample_move_dS_s16;
		break;

	case 4:
		if (driver->interleaved) {
			driver->channel_copy = memcpy_interleave_d32_s32;
		} else {
			driver->channel_copy = memcpy_fake;
		}
		
		driver->write_via_copy = sample_move_d32u24_sS;
		driver->read_via_copy = sample_move_dS_s32u24;

		break;
	}
}

static int
alsa_driver_configure_stream (alsa_driver_t *driver, 
			      const char *stream_name,
			      snd_pcm_t *handle, 
			      snd_pcm_hw_params_t *hw_params, 
			      snd_pcm_sw_params_t *sw_params, 
			      unsigned long *nchns)
{
	int err;

	if ((err = snd_pcm_hw_params_any (handle, hw_params)) < 0)  {
		jack_error ("ALSA: no playback configurations available");
		return -1;
	}

	if ((err = snd_pcm_hw_params_set_periods_integer (handle, hw_params)) < 0) {
		jack_error ("ALSA: cannot restrict period size to integral value.");
		return -1;
	}

	if ((err = snd_pcm_hw_params_set_access (handle, hw_params, SND_PCM_ACCESS_MMAP_NONINTERLEAVED)) < 0) {
		if ((err = snd_pcm_hw_params_set_access (handle, hw_params, SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0) {
			jack_error ("ALSA: mmap-based access is not possible for the %s "
				  "stream of this audio interface", stream_name);
			return -1;
		}
	}
	
	if ((err = snd_pcm_hw_params_set_format (handle, hw_params, SND_PCM_FORMAT_S32_LE)) < 0) {
		if ((err = snd_pcm_hw_params_set_format (handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
			jack_error ("Sorry. The audio interface \"%s\""
				  "doesn't support either of the two hardware sample formats that ardour can use.",
				  driver->alsa_name);
			return -1;
		}
	}

	if ((err = snd_pcm_hw_params_set_rate (handle, hw_params, driver->frame_rate, 0)) < 0) {
		jack_error ("ALSA: cannot set sample/frame rate to %u for %s", driver->frame_rate, stream_name);
		return -1;
	}

	*nchns = snd_pcm_hw_params_get_channels_max (hw_params);

	if (*nchns > 1024) { 
		/* the hapless user is an unwitting victim of the "default"
		   ALSA PCM device, which can support up to 16 million
		   channels. since they can't be bothered to set up
		   a proper default device, limit the number of channels
		   for them to a sane default.
		*/
		*nchns = 2;  
	}				

	if ((err = snd_pcm_hw_params_set_channels (handle, hw_params, *nchns)) < 0) {
		jack_error ("ALSA: cannot set channel count to %u for %s", *nchns, stream_name);
		return -1;
	}
	
	if ((err = snd_pcm_hw_params_set_period_size (handle, hw_params, driver->frames_per_cycle, 0)) < 0) {
		jack_error ("ALSA: cannot set period size to %u frames for %s", driver->frames_per_cycle, stream_name);
		return -1;
	}

	if ((err = snd_pcm_hw_params_set_periods (handle, hw_params, 2, 0)) < 0) {
		jack_error ("ALSA: cannot set number of periods to 2 for %s", stream_name);
		return -1;
	}
	
	if ((err = snd_pcm_hw_params_set_buffer_size (handle, hw_params, 2 * driver->frames_per_cycle)) < 0) {
		jack_error ("ALSA: cannot set buffer length to %u for %s", 2 * driver->frames_per_cycle, stream_name);
		return -1;
	}

	if ((err = snd_pcm_hw_params (handle, hw_params)) < 0) {
		jack_error ("ALSA: cannot set hardware parameters for %s", stream_name);
		return -1;
	}

	snd_pcm_sw_params_current (handle, sw_params);

	if ((err = snd_pcm_sw_params_set_start_threshold (handle, sw_params, ~0U)) < 0) {
		jack_error ("ALSA: cannot set start mode for %s", stream_name);
		return -1;
	}

	if ((err = snd_pcm_sw_params_set_stop_threshold (handle, sw_params, ~0U)) < 0) {
		jack_error ("ALSA: cannot set start mode for %s", stream_name);
		return -1;
	}

	if ((err = snd_pcm_sw_params_set_silence_threshold (handle, sw_params, 0)) < 0) {
		jack_error ("ALSA: cannot set start mode for %s", stream_name);
		return -1;
	}

	if ((err = snd_pcm_sw_params_set_silence_size (handle, sw_params, driver->frames_per_cycle * driver->nfragments)) < 0) {
		jack_error ("ALSA: cannot set start mode for %s", stream_name);
		return -1;
	}

	if ((err = snd_pcm_sw_params_set_avail_min (handle, sw_params, driver->frames_per_cycle)) < 0) {
		jack_error ("ALSA: cannot set avail min for %s", stream_name);
		return -1;
	}

	if ((err = snd_pcm_sw_params (handle, sw_params)) < 0) {
		jack_error ("ALSA: cannot set software parameters for %s", stream_name);
		return -1;
	}

	return 0;
}

static int 
alsa_driver_set_parameters (alsa_driver_t *driver, nframes_t frames_per_cycle, nframes_t rate)

{
	int p_noninterleaved;
	int c_noninterleaved;
	snd_pcm_format_t c_format, p_format;
	int dir;
	unsigned int p_period_size, c_period_size;
	unsigned int p_nfragments, c_nfragments;
	channel_t chn;

	driver->frame_rate = rate;
	driver->frames_per_cycle = frames_per_cycle;
	
	if (alsa_driver_configure_stream (driver, "capture",
					  driver->capture_handle,
					  driver->capture_hw_params,
					  driver->capture_sw_params,
					  &driver->capture_nchannels)) {
		jack_error ("ALSA: cannot configure capture channel");
		return -1;
	}
	
	if (alsa_driver_configure_stream (driver, "playback",
					  driver->playback_handle,
					  driver->playback_hw_params,
					  driver->playback_sw_params,
					  &driver->playback_nchannels)) {
		jack_error ("ALSA: cannot configure playback channel");
		return -1;
	}
	
	/* check the fragment size, since thats non-negotiable */
	
	p_period_size = snd_pcm_hw_params_get_period_size (driver->playback_hw_params, &dir);
	c_period_size = snd_pcm_hw_params_get_period_size (driver->capture_hw_params, &dir);
	
	if (c_period_size != driver->frames_per_cycle || p_period_size != driver->frames_per_cycle) {
		jack_error ("ALSA I/O: requested an interrupt every %u frames but got %uc%up frames",
			  driver->frames_per_cycle, c_period_size, p_period_size);
		return -1;
	}

	p_nfragments = snd_pcm_hw_params_get_periods (driver->playback_hw_params, &dir);
	c_nfragments = snd_pcm_hw_params_get_periods (driver->capture_hw_params, &dir);

	if (p_nfragments != c_nfragments) {
		jack_error ("ALSA I/O: different period counts for playback and capture!");
		return -1;
	}

	driver->nfragments = c_nfragments;
	driver->buffer_frames = driver->frames_per_cycle * driver->nfragments;

	/* Check that we are using the same sample format on both streams */

	p_format = (snd_pcm_format_t) snd_pcm_hw_params_get_format (driver->playback_hw_params);
	c_format = (snd_pcm_format_t) snd_pcm_hw_params_get_format (driver->capture_hw_params);

	if (p_format != c_format) {
		jack_error ("Sorry. The audio interface \"%s\""
			  "doesn't support the same sample format for capture and playback."
			  "Ardour cannot use this hardware.", driver->alsa_name);
		return -1;
	}

	driver->sample_format = p_format;
	driver->sample_bytes = snd_pcm_format_physical_width (driver->sample_format) / 8;
	driver->bytes_per_cycle = driver->sample_bytes * driver->frames_per_cycle;

	switch (driver->sample_format) {
	case SND_PCM_FORMAT_S32_LE:

		/* XXX must handle the n-bits of 24-in-32 problems here */

		if (config_max_level) {
			driver->max_level = config_max_level;
		} else {
			driver->max_level = INT_MAX;
		}

		if (config_min_level) {
			driver->min_level = config_min_level;
		} else {
			driver->min_level = INT_MIN;
		}
		break;

	case SND_PCM_FORMAT_S16_LE:

		if (config_max_level) {
			driver->max_level = config_max_level;
		} else {
			driver->max_level = SHRT_MAX;
		}

		if (config_min_level) {
			driver->min_level = config_min_level;
		} else {
			driver->min_level = SHRT_MIN;
		}
		break;

	default:
		jack_error ("programming error: unhandled format type");
		exit (1);
	}

	/* check interleave setup */

	p_noninterleaved = (snd_pcm_hw_params_get_access (driver->playback_hw_params) == SND_PCM_ACCESS_MMAP_NONINTERLEAVED);
	c_noninterleaved = (snd_pcm_hw_params_get_access (driver->capture_hw_params) == SND_PCM_ACCESS_MMAP_NONINTERLEAVED);

	if (c_noninterleaved != p_noninterleaved) {
		jack_error ("ALSA: the playback and capture components of this audio interface differ "
			  "in their use of channel interleaving. Ardour cannot use this h/w.");
		return -1;
	}

	driver->interleaved = !c_noninterleaved;

	if (driver->interleaved) {
		driver->interleave_unit = snd_pcm_format_physical_width (driver->sample_format) / 8;
		driver->playback_interleave_skip = driver->interleave_unit * driver->playback_nchannels;
		driver->capture_interleave_skip = driver->interleave_unit * driver->capture_nchannels;
	} else {
		driver->interleave_unit = 0;  /* NOT USED */
		driver->playback_interleave_skip = snd_pcm_format_physical_width (driver->sample_format) / 8;
		driver->capture_interleave_skip = driver->playback_interleave_skip;
	}

	if (driver->playback_nchannels > driver->capture_nchannels) {
		driver->max_nchannels = driver->playback_nchannels;
		driver->user_nchannels = driver->capture_nchannels;
	} else {
		driver->max_nchannels = driver->capture_nchannels;
		driver->user_nchannels = driver->playback_nchannels;
	}

	alsa_driver_setup_io_function_pointers (driver);

	/* Allocate and initialize structures that rely on
	   the channels counts.
	*/

	driver->playback_addr = (char **) malloc (sizeof (char *) * driver->playback_nchannels);
	driver->capture_addr = (char **) malloc (sizeof (char *)  * driver->capture_nchannels);

	memset (driver->playback_addr, 0, sizeof (char *) * driver->playback_nchannels);
	memset (driver->capture_addr, 0, sizeof (char *) * driver->capture_nchannels);
	
	driver->silent = (unsigned long *) malloc (sizeof (unsigned long) * driver->playback_nchannels);

	for (chn = 0; chn < driver->playback_nchannels; chn++) {
		driver->silent[chn] = 0;
	}

	driver->input_monitor_requests = (unsigned long *) malloc (sizeof (unsigned long) * driver->max_nchannels);
	memset (driver->input_monitor_requests, 0, sizeof (unsigned long) * driver->max_nchannels);

	driver->clock_sync_data = (ClockSyncStatus *) malloc (sizeof (ClockSyncStatus) * 
							      driver->capture_nchannels > driver->playback_nchannels ?
							      driver->capture_nchannels : driver->playback_nchannels);
	
	/* set up the bit pattern that is used to record which
	   channels require action on every cycle. any bits that are
	   not set after the engine's process() call indicate channels
	   that potentially need to be silenced.  

	   XXX this is limited to <wordsize> channels. Use a bitset
	   type instead.
	*/

	driver->channel_done_bits = 0;
	for (chn = 0; chn < driver->playback_nchannels; chn++) {
		driver->channel_done_bits |= (1<<chn);
	}

	driver->period_interval = (unsigned long) floor ((((float) driver->frames_per_cycle) / driver->frame_rate) * 1000.0);

	if (driver->engine) {
		driver->engine->set_buffer_size (driver->engine, driver->frames_per_cycle);
	}

	return 0;
}	

static int
alsa_driver_reset_parameters (alsa_driver_t *driver, nframes_t frames_per_cycle, nframes_t rate)
{
	/* XXX unregister old ports ? */
	alsa_driver_release_channel_dependent_memory (driver);
	return alsa_driver_set_parameters (driver, frames_per_cycle, rate);
}

static int
alsa_driver_get_channel_addresses (alsa_driver_t *driver,
				   snd_pcm_uframes_t *capture_avail,
				   snd_pcm_uframes_t *playback_avail,
				   snd_pcm_uframes_t *capture_offset,
				   snd_pcm_uframes_t *playback_offset)

{
	unsigned long err;
	channel_t chn;

	if (capture_avail) {
		if ((err = snd_pcm_mmap_begin (driver->capture_handle, &driver->capture_areas,
					       (snd_pcm_uframes_t *) capture_offset, 
					       (snd_pcm_uframes_t *) capture_avail)) < 0) {
			jack_error ("ALSA-HW: %s: mmap areas info error", driver->alsa_name);
			return -1;
		}
		
		for (chn = 0; chn < driver->capture_nchannels; chn++) {
			const snd_pcm_channel_area_t *a = &driver->capture_areas[chn];
			driver->capture_addr[chn] = (char *) a->addr + ((a->first + a->step * *capture_offset) / 8);
		}
	}

	if (playback_avail) {
		if ((err = snd_pcm_mmap_begin (driver->playback_handle, &driver->playback_areas, 
					       (snd_pcm_uframes_t *) playback_offset, 
					       (snd_pcm_uframes_t *) playback_avail)) < 0) {
			jack_error ("ALSA-HW: %s: mmap areas info error ", driver->alsa_name);
			return -1;
		}
		
		for (chn = 0; chn < driver->playback_nchannels; chn++) {
			const snd_pcm_channel_area_t *a = &driver->playback_areas[chn];
			driver->playback_addr[chn] = (char *) a->addr + ((a->first + a->step * *playback_offset) / 8);
		}
	}

	return 0;
}
	
static int
alsa_driver_audio_start (alsa_driver_t *driver)

{
	int err;
	snd_pcm_uframes_t poffset, pavail;
	channel_t chn;

	if ((err = snd_pcm_prepare (driver->playback_handle)) < 0) {
		jack_error ("ALSA-HW: prepare error for playback on \"%s\" (%s)", driver->alsa_name, snd_strerror(err));
		return -1;
	}

	if (driver->capture_and_playback_not_synced) {
		if ((err = snd_pcm_prepare (driver->capture_handle)) < 0) {
			jack_error ("ALSA-HW: prepare error for capture on \"%s\" (%s)", driver->alsa_name, snd_strerror(err));
			return -1;
		}
	}

	if (driver->hw_monitoring) {
		driver->hw->set_input_monitor_mask (driver->hw, driver->input_monitor_mask);
	}

	/* fill playback buffer with zeroes, and mark 
	   all fragments as having data.
	*/
	
	pavail = snd_pcm_avail_update (driver->playback_handle);

	if (pavail != driver->buffer_frames) {
		jack_error ("ALSA-HW: full buffer not available at start");
		return -1;
	}

	if (alsa_driver_get_channel_addresses (driver, 0, &pavail, 0, &poffset)) {
		return -1;
	}

	for (chn = 0; chn < driver->playback_nchannels; chn++) {
		alsa_driver_silence_on_channel (driver, chn, driver->buffer_frames);
	}

	snd_pcm_mmap_commit (driver->playback_handle, poffset, driver->buffer_frames);

	if ((err = snd_pcm_start (driver->playback_handle)) < 0) {
		jack_error ("could not start playback (%s)", snd_strerror (err));
		return -1;
	}

	if (driver->capture_and_playback_not_synced) {
		if ((err = snd_pcm_start (driver->capture_handle)) < 0) {
			jack_error ("could not start capture (%s)", snd_strerror (err));
			return -1;
		}
	}
			
	if (driver->hw_monitoring && (driver->input_monitor_mask || driver->all_monitor_in)) {
		if (driver->all_monitor_in) {
			driver->hw->set_input_monitor_mask (driver->hw, ~0U);
		} else {
			driver->hw->set_input_monitor_mask (driver->hw, driver->input_monitor_mask);
		}
	}

	snd_pcm_poll_descriptors (driver->playback_handle, &driver->pfd, 1);
	driver->pfd.events = POLLOUT | POLLERR;

	return 0;
}

static int
alsa_driver_audio_stop (alsa_driver_t *driver)

{
	int err;

	if ((err = snd_pcm_drop (driver->playback_handle)) < 0) {
		jack_error ("ALSA I/O: channel flush for playback failed (%s)", snd_strerror (err));
		return -1;
	}

	if (driver->capture_and_playback_not_synced) {
		if ((err = snd_pcm_drop (driver->capture_handle)) < 0) {
			jack_error ("ALSA I/O: channel flush for capture failed (%s)", snd_strerror (err));
			return -1;
		}
	}
	
	driver->hw->set_input_monitor_mask (driver->hw, 0);

	return 0;
}

static int
alsa_driver_xrun_recovery (alsa_driver_t *driver)

{
	snd_pcm_sframes_t capture_delay;
	int err;

	if ((err = snd_pcm_delay (driver->capture_handle, &capture_delay))) {
		jack_error ("ALSA I/O: cannot determine capture delay (%s)", snd_strerror (err));
		exit (1);
	}

	fprintf (stderr, "ALSA I/O: xrun of %lu frames, (%.3f msecs)\n", capture_delay,
		 ((float) capture_delay / (float) driver->frame_rate) * 1000.0);
	
#if ENGINE
	if (!engine->xrun_recoverable ()) {
		/* don't report an error here, its distracting */
		return -1;
	} 
#endif

	if (alsa_driver_audio_stop (driver) || alsa_driver_audio_start (driver)) {
		return -1;

	}

	return 0;
}	

static void
alsa_driver_silence_untouched_channels (alsa_driver_t *driver, nframes_t nframes)
	
{
	channel_t chn;

	for (chn = 0; chn < driver->playback_nchannels; chn++) {
		if ((driver->channels_not_done & (1<<chn))) { 
			if (driver->silent[chn] < driver->buffer_frames) {
				alsa_driver_silence_on_channel (driver, chn, nframes);
				driver->silent[chn] += nframes;
			}
		}
	}
}

void 
alsa_driver_set_clock_sync_status (alsa_driver_t *driver, channel_t chn, ClockSyncStatus status)

{
	driver->clock_sync_data[chn] = status;
	jack_driver_clock_sync_notify ((jack_driver_t *) driver, chn, status);
}

static int under_gdb = FALSE;

static int
alsa_driver_wait (alsa_driver_t *driver)

{
	snd_pcm_sframes_t avail = 0;
	snd_pcm_sframes_t contiguous = 0;
	snd_pcm_sframes_t capture_avail = 0;
	snd_pcm_sframes_t playback_avail = 0;
	snd_pcm_uframes_t capture_offset = 0;
	snd_pcm_uframes_t playback_offset = 0;
	int xrun_detected;
	channel_t chn;
	GSList *node;
	sample_t *buffer;

  again:
	if (poll (&driver->pfd, 1, 1000) < 0) {
		if (errno == EINTR) {
			printf ("poll interrupt\n");
				// this happens mostly when run
				// under gdb, or when exiting due to a signal
			if (under_gdb) {
				goto again;
			}
			return 1;
		}
		
		jack_error ("ALSA::Device: poll call failed (%s)", strerror (errno));
		return -1;
	}
	
	driver->time_at_interrupt = current_usecs();
	
	if (driver->pfd.revents & POLLERR) {
		jack_error ("ALSA: poll reports error.");
		return -1;
	}
	
	if (driver->pfd.revents == 0) {
		// timed out, such as when the device is paused
		return 0;
	}
	
	xrun_detected = FALSE;
	
	if ((capture_avail = snd_pcm_avail_update (driver->capture_handle)) < 0) {
		if (capture_avail == -EPIPE) {
			xrun_detected = TRUE;
		} else {
			jack_error ("unknown ALSA avail_update return value (%u)", capture_avail);
		}
	}

	if ((playback_avail = snd_pcm_avail_update (driver->playback_handle)) < 0) {
		if (playback_avail == -EPIPE) {
			xrun_detected = TRUE;
		} else {
			jack_error ("unknown ALSA avail_update return value (%u)", playback_avail);
		}
	}

	if (xrun_detected) {
		if (alsa_driver_xrun_recovery (driver)) {
			return -1;
		} else {
			return 0;
		}
	}
	
	avail = capture_avail < playback_avail ? capture_avail : playback_avail;

	while (avail) {
		
		capture_avail = (avail > driver->frames_per_cycle) ? driver->frames_per_cycle : avail;
		playback_avail = (avail > driver->frames_per_cycle) ? driver->frames_per_cycle : avail;
		
		if (alsa_driver_get_channel_addresses (driver, 
						       (snd_pcm_uframes_t *) &capture_avail, 
						       (snd_pcm_uframes_t *) &playback_avail,
						       &capture_offset, &playback_offset) < 0) {
			return -1;
		}

		contiguous = capture_avail < playback_avail ? capture_avail : playback_avail;

		/* XXX possible race condition here with silence_pending */
		
		/* XXX this design is wrong. cf. ardour/audioengine *** FIX ME *** */

		if (driver->silence_pending) {
			for (chn = 0; chn < driver->playback_nchannels; chn++) {
				if (driver->silence_pending & (1<<chn)) {
					alsa_driver_silence_on_channel (driver, chn, contiguous);
				}
			}
			driver->silence_pending = 0;
		}
		
		driver->channels_not_done = driver->channel_done_bits;
		
		if ((driver->hw->input_monitor_mask != driver->input_monitor_mask) && 
		    driver->hw_monitoring && !driver->all_monitor_in) {
			driver->hw->set_input_monitor_mask (driver->hw, driver->input_monitor_mask);
		} 
		
		/* XXX race condition on engine ptr */

		if (driver->engine && driver->engine->process (driver->engine, contiguous)) {
			jack_error ("ALSA I/O: engine processing error - stopping.");
			return -1;
		}

		/* now move data from ports to channels */

		for (chn = 0, node = driver->playback_ports; node; node = g_slist_next (node), chn++) {

			jack_port_t *port = (jack_port_t *) node->data;

			/* optimize needless data copying away */

			if (port->connections == 0) {
				continue;
			}

			buffer = (sample_t *) jack_port_get_buffer (port, contiguous);
			alsa_driver_write_to_channel (driver, chn, buffer, contiguous, 0, 1.0);
		}

		/* Now handle input monitoring */
		
		if (!driver->hw_monitoring) {
			if (driver->all_monitor_in) {
				for (chn = 0; chn < driver->playback_nchannels; chn++) {
					alsa_driver_copy_channel (driver, chn, chn, contiguous);
				}
			} else if (driver->input_monitor_mask) {
				for (chn = 0; chn < driver->playback_nchannels; chn++) {
					if (driver->input_monitor_mask & (1<<chn)) {
						alsa_driver_copy_channel (driver, chn, chn, contiguous);
					}
				}
			}
		}

		if (driver->channels_not_done) {
			alsa_driver_silence_untouched_channels (driver, contiguous);
		}
		
		snd_pcm_mmap_commit (driver->capture_handle, capture_offset, contiguous);
		snd_pcm_mmap_commit (driver->playback_handle, playback_offset, contiguous);
		
		avail -= contiguous;
	}

	return 0;
}

static int
alsa_driver_process (nframes_t nframes, void *arg)

{
	alsa_driver_t *driver = (alsa_driver_t *) arg;
	channel_t chn;
	jack_port_t *port;
	GSList *node;

	for (chn = 0, node = driver->capture_ports; node; node = g_slist_next (node), chn++) {

		port = (jack_port_t *) node->data;

		if (port->connections == 0) {
			continue;
		}

		alsa_driver_read_from_channel (driver, chn, port->shared->buffer, nframes, 0);
	}

	return 0;
}

static void
alsa_driver_port_monitor_handler (jack_port_id_t port_id, int onoff, void *arg)
{
	alsa_driver_t *driver = (alsa_driver_t *) arg;
	jack_port_shared_t *port;
	int channel;

	port = &driver->engine->control->ports[port_id];
	sscanf (port->name, "%*s%*s%*s%d", &channel);
	driver->request_monitor_input ((jack_driver_t *) driver, channel, onoff);
}

static void
alsa_driver_attach (alsa_driver_t *driver, jack_engine_t *engine)

{
	char buf[32];
	channel_t chn;
	jack_port_t *port;

	driver->engine = engine;

	driver->engine->set_buffer_size (engine, driver->frames_per_cycle);
	driver->engine->set_sample_rate (engine, driver->frame_rate);

	/* Now become a client of the engine */

	if ((driver->client = jack_driver_become_client ("ALSA I/O")) == NULL) {
		jack_error ("ALSA: cannot become client");
		return;
	}

	jack_set_process_callback (driver->client, alsa_driver_process, driver);
	jack_set_port_monitor_callback (driver->client, alsa_driver_port_monitor_handler, driver);

	for (chn = 0; chn < driver->capture_nchannels; chn++) {
		snprintf (buf, sizeof(buf) - 1, "Input %lu", chn+1);
		port = jack_port_register (driver->client, buf, 
					   JACK_DEFAULT_AUDIO_TYPE,
					   JackPortIsOutput|JackPortIsPhysical|JackPortCanMonitor, 0);
		if (port == 0) {
			jack_error ("ALSA: cannot register port for %s", buf);
			break;
		}
		driver->capture_ports = g_slist_append (driver->capture_ports, port);
		printf ("registered %s\n", port->shared->name);
	}

	for (chn = 0; chn < driver->playback_nchannels; chn++) {
		snprintf (buf, sizeof(buf) - 1, "Output %lu", chn+1);
		port = jack_port_register (driver->client, buf, 
					    JACK_DEFAULT_AUDIO_TYPE,
					    JackPortIsInput|JackPortIsPhysical, 0);
		if (port == 0) {
			jack_error ("ALSA: cannot register port for %s", buf);
			break;
		}
		driver->playback_ports = g_slist_append (driver->playback_ports, port);
		printf ("registered %s\n", port->shared->name);
	}

	printf ("ports registered, starting client\n");

	jack_activate (driver->client);
}

static void
alsa_driver_detach (alsa_driver_t *driver, jack_engine_t *engine)

{
	GSList *node;

	for (node = driver->capture_ports; node; node = g_slist_next (node)) {
		jack_port_unregister (driver->client, ((jack_port_t *) node->data));
	}

	g_slist_free (driver->capture_ports);
	driver->capture_ports = 0;
		
	for (node = driver->playback_ports; node; node = g_slist_next (node)) {
		jack_port_unregister (driver->client, ((jack_port_t *) node->data));
	}

	g_slist_free (driver->playback_ports);
	driver->playback_ports = 0;
	
	driver->engine = 0;
}

static int
alsa_driver_change_sample_clock (alsa_driver_t *driver, SampleClockMode mode)

{
	return driver->hw->change_sample_clock (driver->hw, mode);
}

static void
alsa_driver_mark_channel_silent (alsa_driver_t *driver, unsigned long chn)
{
	driver->silence_pending |= (1<<chn);
}

static void
alsa_driver_request_monitor_input (alsa_driver_t *driver, unsigned long chn, int yn)
	
{
	int changed;

	if (chn >= driver->max_nchannels) {
		return;
	}

	changed = FALSE;

	if (yn) {
		if (++driver->input_monitor_requests[chn] == 1) {
			if (!(driver->input_monitor_mask & (1<<chn))) {
				driver->input_monitor_mask |= (1<<chn);
				changed = TRUE;
			}
		}
	} else {
		if (driver->input_monitor_requests[chn] && --driver->input_monitor_requests[chn] == 0) {
			if (driver->input_monitor_mask & (1<<chn)) {
				driver->input_monitor_mask &= ~(1<<chn);
				changed = TRUE;
			}
		}
	}

	if (changed) {
		if (!driver->hw_monitoring && !yn) {
			alsa_driver_mark_channel_silent (driver, chn);
		}

		/* Tell anyone who cares about the state of input monitoring */
		
		jack_driver_input_monitor_notify ((jack_driver_t *) driver, chn, yn);
	}
}

static void
alsa_driver_request_all_monitor_input (alsa_driver_t *driver, int yn)

{
	if (driver->hw_monitoring) {
		if (yn) {
			driver->hw->set_input_monitor_mask (driver->hw, ~0U);
		} else {
			driver->hw->set_input_monitor_mask (driver->hw, driver->input_monitor_mask);
		}
	}

	driver->all_monitor_in = yn;
}

static void
alsa_driver_set_hw_monitoring (alsa_driver_t *driver, int yn)

{
	if (yn) {
		driver->hw_monitoring = TRUE;
		
		if (driver->all_monitor_in) {
			driver->hw->set_input_monitor_mask (driver->hw, ~0U);
		} else {
			driver->hw->set_input_monitor_mask (driver->hw, driver->input_monitor_mask);
		}
	} else {
		driver->hw_monitoring = FALSE;
		driver->hw->set_input_monitor_mask (driver->hw, 0);
	}
}

static nframes_t
alsa_driver_frames_since_cycle_start (alsa_driver_t *driver)
{
	return (nframes_t) ((driver->frame_rate / 1000000.0) * ((float) (current_usecs() - driver->time_at_interrupt)));
}

static ClockSyncStatus
alsa_driver_clock_sync_status (channel_t chn)

{
	return Lock;
}

static void
alsa_driver_delete (alsa_driver_t *driver)

{
	if (driver->capture_handle) {
		snd_pcm_close (driver->capture_handle);
		driver->capture_handle = 0;
	} 

	if (driver->playback_handle) {
		snd_pcm_close (driver->playback_handle);
		driver->capture_handle = 0;
	}
	
	if (driver->capture_hw_params) {
		snd_pcm_hw_params_free (driver->capture_hw_params);
		driver->capture_hw_params = 0;
	}

	if (driver->playback_hw_params) {
		snd_pcm_hw_params_free (driver->playback_hw_params);
		driver->playback_hw_params = 0;
	}
	
	if (driver->capture_sw_params) {
		snd_pcm_sw_params_free (driver->capture_sw_params);
		driver->capture_sw_params = 0;
	}
	
	if (driver->playback_sw_params) {
		snd_pcm_sw_params_free (driver->playback_sw_params);
		driver->playback_sw_params = 0;
	}
	
	if (driver->hw) {
		driver->hw->release (driver->hw);
		driver->hw = 0;
	}
	free(driver->alsa_name);
	free(driver->alsa_driver);

	alsa_driver_release_channel_dependent_memory (driver);
	jack_driver_release ((jack_driver_t *) driver);
	free (driver);
}

static jack_driver_t *
alsa_driver_new (char *name, char *alsa_device,
		 nframes_t frames_per_cycle,
		 nframes_t rate)
{
	int err;

	alsa_driver_t *driver;

	printf ("creating alsa driver ... %s|%lu|%lu\n", alsa_device, frames_per_cycle, rate);

	driver = (alsa_driver_t *) calloc (1, sizeof (alsa_driver_t));

	jack_driver_init ((jack_driver_t *) driver);

	driver->attach = (JackDriverAttachFunction) alsa_driver_attach;
        driver->detach = (JackDriverDetachFunction) alsa_driver_detach;
	driver->wait = (JackDriverWaitFunction) alsa_driver_wait;

	driver->audio_stop = (JackDriverAudioStopFunction) alsa_driver_audio_stop;
	driver->audio_start = (JackDriverAudioStartFunction) alsa_driver_audio_start;
	driver->set_hw_monitoring  = (JackDriverSetHwMonitoringFunction) alsa_driver_set_hw_monitoring ;
	driver->reset_parameters  = (JackDriverResetParametersFunction) alsa_driver_reset_parameters;
	driver->mark_channel_silent  = (JackDriverMarkChannelSilentFunction) alsa_driver_mark_channel_silent;
	driver->request_monitor_input  = (JackDriverRequestMonitorInputFunction) alsa_driver_request_monitor_input;
	driver->request_all_monitor_input = (JackDriverRequestAllMonitorInputFunction) alsa_driver_request_all_monitor_input;
	driver->frames_since_cycle_start = (JackDriverFramesSinceCycleStartFunction) alsa_driver_frames_since_cycle_start;
	driver->clock_sync_status = (JackDriverClockSyncStatusFunction) alsa_driver_clock_sync_status;
	driver->change_sample_clock  = (JackDriverChangeSampleClockFunction) alsa_driver_change_sample_clock;

	driver->ctl_handle = 0;
	driver->hw = 0;
	driver->capture_and_playback_not_synced = FALSE;
	driver->nfragments = 0;
	driver->max_nchannels = 0;
	driver->user_nchannels = 0;
	driver->playback_nchannels = 0;
	driver->capture_nchannels = 0;
	driver->playback_addr = 0;
	driver->capture_addr = 0;
	driver->silence_pending = 0;
	driver->silent = 0;
	driver->input_monitor_requests = 0;
	driver->all_monitor_in = FALSE;

	driver->clock_mode = ClockMaster; /* XXX is it? */
	driver->input_monitor_mask = 0;   /* XXX is it? */
	
	driver->capture_ports = 0;
	driver->playback_ports = 0;
	
	if ((err = snd_pcm_open (&driver->playback_handle, alsa_device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		jack_error ("ALSA: Cannot open PCM device %s/%s", name, alsa_device);
		free (driver);
		return 0;
	}

	driver->alsa_name = strdup (alsa_device);

	if ((err = snd_pcm_open (&driver->capture_handle, alsa_device, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		jack_error ("ALSA: Cannot open PCM device %s", name);
		free (driver);
		return 0;
	}

	if (alsa_driver_check_card_type (driver)) {
		free (driver);
		return 0;
	}

	driver->playback_hw_params = 0;
	driver->capture_hw_params = 0;
	driver->playback_sw_params = 0;
	driver->capture_hw_params = 0;

	if ((err = snd_pcm_hw_params_malloc (&driver->playback_hw_params)) < 0) {
		jack_error ("ALSA: could no allocate playback hw params structure");
		alsa_driver_delete (driver);
		return 0;
	}

	if ((err = snd_pcm_hw_params_malloc (&driver->capture_hw_params)) < 0) {
		jack_error ("ALSA: could no allocate capture hw params structure");
		alsa_driver_delete (driver);
		return 0;
	}

	if ((err = snd_pcm_sw_params_malloc (&driver->playback_sw_params)) < 0) {
		jack_error ("ALSA: could no allocate playback sw params structure");
		alsa_driver_delete (driver);
		return 0;
	}

	if ((err = snd_pcm_sw_params_malloc (&driver->capture_sw_params)) < 0) {
		jack_error ("ALSA: could no allocate capture sw params structure");
		alsa_driver_delete (driver);
		return 0;
	}

	if (alsa_driver_set_parameters (driver, frames_per_cycle, rate)) {
		alsa_driver_delete (driver);
		return 0;
	}

	if (snd_pcm_link (driver->capture_handle, driver->playback_handle) != 0) {
		driver->capture_and_playback_not_synced = TRUE;
	} else {
		driver->capture_and_playback_not_synced = FALSE;
	}

	alsa_driver_hw_specific (driver);

	return (jack_driver_t *) driver;
}

/* PLUGIN INTERFACE */

jack_driver_t *
driver_initialize (va_list ap)
{
	nframes_t srate;
	nframes_t frames_per_interrupt;
	char *pcm_name;

	pcm_name = va_arg (ap, char *);
	frames_per_interrupt = va_arg (ap, nframes_t);
	srate = va_arg (ap, nframes_t);

	return alsa_driver_new ("ALSA I/O", pcm_name, frames_per_interrupt, srate);
}

void
driver_finish (jack_driver_t *driver)
{
	alsa_driver_delete ((alsa_driver_t *) driver);
}
