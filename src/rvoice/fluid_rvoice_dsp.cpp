/* FluidSynth - A Software Synthesizer
 *
 * Copyright (C) 2003  Peter Hanappe and others.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "fluid_sys.h"
#include "fluid_phase.h"
#include "fluid_rvoice.h"
#include "fluid_rvoice_dsp_tables.inc.h"

/* Purpose:
 *
 * Interpolates audio data (obtains values between the samples of the original
 * waveform data).
 *
 * Variables loaded from the voice structure (assigned in fluid_rvoice_write()):
 * - dsp_data: Pointer to the original waveform data
 * - dsp_phase: The position in the original waveform data.
 *              This has an integer and a fractional part (between samples).
 * - dsp_phase_incr: For each output sample, the position in the original
 *              waveform advances by dsp_phase_incr. This also has an integer
 *              part and a fractional part.
 *              If a sample is played at root pitch (no pitch change),
 *              dsp_phase_incr is integer=1 and fractional=0.
 * - dsp_amp: The current amplitude envelope value.
 * - dsp_amp_incr: The changing rate of the amplitude envelope.
 *
 * A couple of variables are used internally, their results are discarded:
 * - dsp_i: Index through the output buffer
 * - dsp_buf: Output buffer of floating point values (FLUID_BUFSIZE in length)
 */

/* Interpolation (find a value between two samples of the original waveform) */

template<bool IS_24BIT>
static FLUID_INLINE fluid_real_t
fluid_rvoice_get_float_sample(const short int *FLUID_RESTRICT dsp_msb, const char *FLUID_RESTRICT dsp_lsb, unsigned int idx)
{
    int32_t sample;
    if (IS_24BIT)
    {
        sample = fluid_rvoice_get_sample24(dsp_msb, dsp_lsb, idx);
    }
    else
    {
        sample = fluid_rvoice_get_sample16(dsp_msb, idx);
    }
    
    return (fluid_real_t)sample;
}

/* Special case of interpolate_none for rendering silent voices, i.e. in delay phase or zero volume */
template<bool LOOPING>
static int fluid_rvoice_dsp_silence_local(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf)
{
    fluid_rvoice_dsp_t *voice = &rvoice->dsp;
    fluid_phase_t dsp_phase = voice->phase;
    fluid_phase_t dsp_phase_incr;
    unsigned short dsp_i = 0;
    unsigned int dsp_phase_index;
    unsigned int end_index;

    /* Convert playback "speed" floating point value to phase index/fract */
    fluid_phase_set_float(dsp_phase_incr, voice->phase_incr);

    end_index = LOOPING ? voice->loopend - 1 : voice->end;

    while (1)
    {
        dsp_phase_index = fluid_phase_index_round(dsp_phase); /* round to nearest point */

        /* interpolate sequence of sample points */
        for (; dsp_i < FLUID_BUFSIZE && dsp_phase_index <= end_index; dsp_i++)
        {
            fluid_real_t sample = 0;
            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
        }

        /* break out if not looping (buffer may not be full) */
        if (!LOOPING)
        {
            break;
        }

        dsp_phase_index = fluid_phase_index_round(dsp_phase); /* round to nearest point */
        /* go back to loop start */
        if (dsp_phase_index > end_index)
        {
            fluid_phase_sub_int(dsp_phase, voice->loopend - voice->loopstart);
            voice->has_looped = 1;
        }

        /* break out if filled buffer */
        if (dsp_i >= FLUID_BUFSIZE)
        {
            break;
        }
    }

    voice->phase = dsp_phase;
    // Note, there is no need to update the amplitude here. When the voice becomes audible again, the amp will be updated anyway in fluid_rvoice_calc_amp().
    // voice->amp = dsp_amp;

    return (dsp_i);
}

/* No interpolation. Just take the sample, which is closest to
  * the playback pointer.  Questionable quality, but very
  * efficient. */
template<bool IS_24BIT, bool LOOPING>
static int
fluid_rvoice_dsp_interpolate_none_local(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf)
{
    fluid_rvoice_dsp_t *voice = &rvoice->dsp;
    fluid_phase_t dsp_phase = voice->phase;
    fluid_phase_t dsp_phase_incr;
    const short int *FLUID_RESTRICT dsp_data = voice->sample->data;
    const char *FLUID_RESTRICT dsp_data24 = voice->sample->data24;
    unsigned short dsp_i = 0;
    unsigned int dsp_phase_index;
    unsigned int end_index;

    /* Convert playback "speed" floating point value to phase index/fract */
    fluid_phase_set_float(dsp_phase_incr, voice->phase_incr);

    end_index = LOOPING ? voice->loopend - 1 : voice->end;

    while(1)
    {
        dsp_phase_index = fluid_phase_index_round(dsp_phase);	/* round to nearest point */

        /* interpolate sequence of sample points */
        for(; dsp_i < FLUID_BUFSIZE && dsp_phase_index <= end_index; dsp_i++)
        {
            fluid_real_t sample = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index);
            
            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index_round(dsp_phase);	/* round to nearest point */
        }

        /* break out if not looping (buffer may not be full) */
        if(!LOOPING)
        {
            break;
        }

        /* go back to loop start */
        if(dsp_phase_index > end_index)
        {
            fluid_phase_sub_int(dsp_phase, voice->loopend - voice->loopstart);
            voice->has_looped = 1;
        }

        /* break out if filled buffer */
        if(dsp_i >= FLUID_BUFSIZE)
        {
            break;
        }
    }

    voice->phase = dsp_phase;

    return (dsp_i);
}

/* Straight line interpolation.
 * Returns number of samples processed (usually FLUID_BUFSIZE but could be
 * smaller if end of sample occurs).
 */
template<bool IS_24BIT, bool LOOPING>
static int
fluid_rvoice_dsp_interpolate_linear_local(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf)
{
    fluid_rvoice_dsp_t *voice = &rvoice->dsp;
    fluid_phase_t dsp_phase = voice->phase;
    fluid_phase_t dsp_phase_incr;
    const short int *FLUID_RESTRICT dsp_data = voice->sample->data;
    const char *FLUID_RESTRICT dsp_data24 = voice->sample->data24;
    unsigned short dsp_i = 0;
    unsigned int dsp_phase_index;
    unsigned int end_index;
    fluid_real_t point;
    const fluid_real_t *FLUID_RESTRICT coeffs;

    /* Convert playback "speed" floating point value to phase index/fract */
    fluid_phase_set_float(dsp_phase_incr, voice->phase_incr);

    /* last index before 2nd interpolation point must be specially handled */
    end_index = (LOOPING ? voice->loopend - 1 : voice->end) - 1;

    /* 2nd interpolation point to use at end of loop or sample */
    if(LOOPING)
    {
        point = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopstart);    /* loop start */
    }
    else
    {
        point = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->end);    /* duplicate end for samples no longer looping */
    }

    while(1)
    {
        dsp_phase_index = fluid_phase_index(dsp_phase);

        /* interpolate the sequence of sample points */
        for(; dsp_i < FLUID_BUFSIZE && dsp_phase_index <= end_index; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = interp_coeff_linear[fluid_phase_fract_to_tablerow(dsp_phase)];
            
            sample =  (coeffs[0] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[1] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 1));
                        
            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        /* break out if buffer filled */
        if(dsp_i >= FLUID_BUFSIZE)
        {
            break;
        }

        end_index++;	/* we're now interpolating the last point */

        /* interpolate within last point */
        for(; dsp_phase_index <= end_index && dsp_i < FLUID_BUFSIZE; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = interp_coeff_linear[fluid_phase_fract_to_tablerow(dsp_phase)];
            
            sample =  (coeffs[0] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[1] * point);

            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        if(!LOOPING)
        {
            break;    /* break out if not looping (end of sample) */
        }

        /* go back to loop start (if past */
        if(dsp_phase_index > end_index)
        {
            fluid_phase_sub_int(dsp_phase, voice->loopend - voice->loopstart);
            voice->has_looped = 1;
        }

        /* break out if filled buffer */
        if(dsp_i >= FLUID_BUFSIZE)
        {
            break;
        }

        end_index--;	/* set end back to second to last sample point */
    }

    voice->phase = dsp_phase;

    return (dsp_i);
}

/* 4th order (cubic) interpolation.
 * Returns number of samples processed (usually FLUID_BUFSIZE but could be
 * smaller if end of sample occurs).
 */
template<bool IS_24BIT, bool LOOPING>
static int
fluid_rvoice_dsp_interpolate_4th_order_local(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf)
{
    fluid_rvoice_dsp_t *voice = &rvoice->dsp;
    fluid_phase_t dsp_phase = voice->phase;
    fluid_phase_t dsp_phase_incr;
    const short int *FLUID_RESTRICT dsp_data = voice->sample->data;
    const char *FLUID_RESTRICT dsp_data24 = voice->sample->data24;
    unsigned short dsp_i = 0;
    unsigned int dsp_phase_index;
    unsigned int start_index, end_index;
    fluid_real_t start_point, end_point1, end_point2;
    const fluid_real_t *FLUID_RESTRICT coeffs;

    /* Convert playback "speed" floating point value to phase index/fract */
    fluid_phase_set_float(dsp_phase_incr, voice->phase_incr);

    /* last index before 4th interpolation point must be specially handled */
    end_index = (LOOPING ? voice->loopend - 1 : voice->end) - 2;

    if(voice->has_looped)	/* set start_index and start point if looped or not */
    {
        start_index = voice->loopstart;
        start_point = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopend - 1);	/* last point in loop (wrap around) */
    }
    else
    {
        start_index = voice->start;
        start_point = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->start);	/* just duplicate the point */
    }

    /* get points off the end (loop start if looping, duplicate point if end) */
    if(LOOPING)
    {
        end_point1 = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopstart);
        end_point2 = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopstart + 1);
    }
    else
    {
        end_point1 = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->end);
        end_point2 = end_point1;
    }

    while(1)
    {
        dsp_phase_index = fluid_phase_index(dsp_phase);

        /* interpolate first sample point (start or loop start) if needed */
        for(; dsp_phase_index == start_index && dsp_i < FLUID_BUFSIZE; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = interp_coeff[fluid_phase_fract_to_tablerow(dsp_phase)];

            sample =  (coeffs[0] * start_point
                     + coeffs[1] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[2] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 1)
                     + coeffs[3] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 2));
                        
            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        /* interpolate the sequence of sample points */
        for(; dsp_i < FLUID_BUFSIZE && dsp_phase_index <= end_index; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = interp_coeff[fluid_phase_fract_to_tablerow(dsp_phase)];

            sample =  (coeffs[0] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 1)
                     + coeffs[1] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[2] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 1)
                     + coeffs[3] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 2));

            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        /* break out if buffer filled */
        if(dsp_i >= FLUID_BUFSIZE)
        {
            break;
        }

        end_index++;	/* we're now interpolating the 2nd to last point */

        /* interpolate within 2nd to last point */
        for(; dsp_phase_index <= end_index && dsp_i < FLUID_BUFSIZE; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = interp_coeff[fluid_phase_fract_to_tablerow(dsp_phase)];

            sample =  (coeffs[0] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 1)
                     + coeffs[1] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[2] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 1)
                     + coeffs[3] * end_point1);

            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        end_index++;	/* we're now interpolating the last point */

        /* interpolate within the last point */
        for(; dsp_phase_index <= end_index && dsp_i < FLUID_BUFSIZE; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = interp_coeff[fluid_phase_fract_to_tablerow(dsp_phase)];

            
            sample =  (coeffs[0] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 1)
                     + coeffs[1] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[2] * end_point1
                     + coeffs[3] * end_point2);

            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        if(!LOOPING)
        {
            break;    /* break out if not looping (end of sample) */
        }

        /* go back to loop start */
        if(dsp_phase_index > end_index)
        {
            fluid_phase_sub_int(dsp_phase, voice->loopend - voice->loopstart);

            if(!voice->has_looped)
            {
                voice->has_looped = 1;
                start_index = voice->loopstart;
                start_point = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopend - 1);
            }
        }

        /* break out if filled buffer */
        if(dsp_i >= FLUID_BUFSIZE)
        {
            break;
        }

        end_index -= 2;	/* set end back to third to last sample point */
    }

    voice->phase = dsp_phase;

    return (dsp_i);
}

/* 7th order interpolation.
 * Returns number of samples processed (usually FLUID_BUFSIZE but could be
 * smaller if end of sample occurs).
 */
template<bool IS_24BIT, bool LOOPING>
static int
fluid_rvoice_dsp_interpolate_7th_order_local(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf)
{
    fluid_rvoice_dsp_t *voice = &rvoice->dsp;
    fluid_phase_t dsp_phase = voice->phase;
    fluid_phase_t dsp_phase_incr;
    const short int *FLUID_RESTRICT dsp_data = voice->sample->data;
    const char *FLUID_RESTRICT dsp_data24 = voice->sample->data24;
    unsigned short dsp_i = 0;
    unsigned int dsp_phase_index;
    unsigned int start_index, end_index;
    fluid_real_t start_points[3], end_points[3];
    const fluid_real_t *FLUID_RESTRICT coeffs;

    /* Convert playback "speed" floating point value to phase index/fract */
    fluid_phase_set_float(dsp_phase_incr, voice->phase_incr);

    /* add 1/2 sample to dsp_phase since 7th order interpolation is centered on
     * the 4th sample point */
    fluid_phase_incr(dsp_phase, (fluid_phase_t)0x80000000);

    /* last index before 7th interpolation point must be specially handled */
    end_index = (LOOPING ? voice->loopend - 1 : voice->end) - 3;

    if(voice->has_looped)	/* set start_index and start point if looped or not */
    {
        start_index = voice->loopstart;
        start_points[0] = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopend - 1);
        start_points[1] = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopend - 2);
        start_points[2] = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopend - 3);
    }
    else
    {
        start_index = voice->start;
        start_points[0] = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->start);	/* just duplicate the start point */
        start_points[1] = start_points[0];
        start_points[2] = start_points[0];
    }

    /* get the 3 points off the end (loop start if looping, duplicate point if end) */
    if(LOOPING)
    {
        end_points[0] = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopstart);
        end_points[1] = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopstart + 1);
        end_points[2] = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopstart + 2);
    }
    else
    {
        end_points[0] = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->end);
        end_points[1] = end_points[0];
        end_points[2] = end_points[0];
    }

    while(1)
    {
        dsp_phase_index = fluid_phase_index(dsp_phase);

        /* interpolate first sample point (start or loop start) if needed */
        for(; dsp_phase_index == start_index && dsp_i < FLUID_BUFSIZE; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = sinc_table7[fluid_phase_fract_to_tablerow(dsp_phase)];

            sample =  (coeffs[0] * start_points[2]
                     + coeffs[1] * start_points[1]
                     + coeffs[2] * start_points[0]
                     + coeffs[3] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[4] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 1)
                     + coeffs[5] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 2)
                     + coeffs[6] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 3));

            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        start_index++;

        /* interpolate 2nd to first sample point (start or loop start) if needed */
        for(; dsp_phase_index == start_index && dsp_i < FLUID_BUFSIZE; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = sinc_table7[fluid_phase_fract_to_tablerow(dsp_phase)];

            sample =  (coeffs[0] * start_points[1]
                     + coeffs[1] * start_points[0]
                     + coeffs[2] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 1)
                     + coeffs[3] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[4] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 1)
                     + coeffs[5] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 2)
                     + coeffs[6] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 3));

            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        start_index++;

        /* interpolate 3rd to first sample point (start or loop start) if needed */
        for(; dsp_phase_index == start_index && dsp_i < FLUID_BUFSIZE; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = sinc_table7[fluid_phase_fract_to_tablerow(dsp_phase)];

            sample =  (coeffs[0] * start_points[0]
                     + coeffs[1] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 2)
                     + coeffs[2] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 1)
                     + coeffs[3] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[4] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 1)
                     + coeffs[5] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 2)
                     + coeffs[6] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 3));

            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        start_index -= 2;	/* set back to original start index */


        /* interpolate the sequence of sample points */
        for(; dsp_i < FLUID_BUFSIZE && dsp_phase_index <= end_index; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = sinc_table7[fluid_phase_fract_to_tablerow(dsp_phase)];

            sample =  (coeffs[0] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 3)
                     + coeffs[1] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 2)
                     + coeffs[2] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 1)
                     + coeffs[3] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[4] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 1)
                     + coeffs[5] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 2)
                     + coeffs[6] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 3));

            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        /* break out if buffer filled */
        if(dsp_i >= FLUID_BUFSIZE)
        {
            break;
        }

        end_index++;	/* we're now interpolating the 3rd to last point */

        /* interpolate within 3rd to last point */
        for(; dsp_phase_index <= end_index && dsp_i < FLUID_BUFSIZE; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = sinc_table7[fluid_phase_fract_to_tablerow(dsp_phase)];

            sample =  (coeffs[0] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 3)
                     + coeffs[1] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 2)
                     + coeffs[2] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 1)
                     + coeffs[3] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[4] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 1)
                     + coeffs[5] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 2)
                     + coeffs[6] * end_points[0]);

            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        end_index++;	/* we're now interpolating the 2nd to last point */

        /* interpolate within 2nd to last point */
        for(; dsp_phase_index <= end_index && dsp_i < FLUID_BUFSIZE; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = sinc_table7[fluid_phase_fract_to_tablerow(dsp_phase)];

            sample =  (coeffs[0] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 3)
                     + coeffs[1] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 2)
                     + coeffs[2] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 1)
                     + coeffs[3] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[4] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index + 1)
                     + coeffs[5] * end_points[0]
                     + coeffs[6] * end_points[1]);

            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        end_index++;	/* we're now interpolating the last point */

        /* interpolate within last point */
        for(; dsp_phase_index <= end_index && dsp_i < FLUID_BUFSIZE; dsp_i++)
        {
            fluid_real_t sample;
            coeffs = sinc_table7[fluid_phase_fract_to_tablerow(dsp_phase)];

            sample =  (coeffs[0] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 3)
                     + coeffs[1] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 2)
                     + coeffs[2] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index - 1)
                     + coeffs[3] * fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, dsp_phase_index)
                     + coeffs[4] * end_points[0]
                     + coeffs[5] * end_points[1]
                     + coeffs[6] * end_points[2]);

            dsp_buf[dsp_i] = sample;

            /* increment phase and amplitude */
            fluid_phase_incr(dsp_phase, dsp_phase_incr);
            dsp_phase_index = fluid_phase_index(dsp_phase);
        }

        if(!LOOPING)
        {
            break;    /* break out if not looping (end of sample) */
        }

        /* go back to loop start */
        if(dsp_phase_index > end_index)
        {
            fluid_phase_sub_int(dsp_phase, voice->loopend - voice->loopstart);

            if(!voice->has_looped)
            {
                voice->has_looped = 1;
                start_index = voice->loopstart;
                start_points[0] = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopend - 1);
                start_points[1] = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopend - 2);
                start_points[2] = fluid_rvoice_get_float_sample<IS_24BIT>(dsp_data, dsp_data24, voice->loopend - 3);
            }
        }

        /* break out if filled buffer */
        if(dsp_i >= FLUID_BUFSIZE)
        {
            break;
        }

        end_index -= 3;	/* set end back to 4th to last sample point */
    }

    /* sub 1/2 sample from dsp_phase since 7th order interpolation is centered on
     * the 4th sample point (correct back to real value) */
    fluid_phase_decr(dsp_phase, (fluid_phase_t)0x80000000);

    voice->phase = dsp_phase;

    return (dsp_i);
}

struct ProcessSilence
{
    template<bool IS_24BIT, bool LOOPING>
    int operator()(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf) const
    {
        return fluid_rvoice_dsp_silence_local<LOOPING>(rvoice, dsp_buf);
    }
};

struct InterpolateNone
{
    template<bool IS_24BIT, bool LOOPING>
    int operator()(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf) const
    {
        return fluid_rvoice_dsp_interpolate_none_local<IS_24BIT, LOOPING>(rvoice, dsp_buf);
    }
};

struct InterpolateLinear
{
    template<bool IS_24BIT, bool LOOPING>
    int operator()(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf) const
    {
        return fluid_rvoice_dsp_interpolate_linear_local<IS_24BIT, LOOPING>(rvoice, dsp_buf);
    }
};

struct Interpolate4thOrder
{
    template<bool IS_24BIT, bool LOOPING>
    int operator()(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf) const
    {
        return fluid_rvoice_dsp_interpolate_4th_order_local<IS_24BIT, LOOPING>(rvoice, dsp_buf);
    }
};

struct Interpolate7thOrder
{
    template<bool IS_24BIT, bool LOOPING>
    int operator()(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf) const
    {
        return fluid_rvoice_dsp_interpolate_7th_order_local<IS_24BIT, LOOPING>(rvoice, dsp_buf);
    }
};

template<typename T>
int dsp_invoker(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf, int looping)
{
    T func;
    bool is_24bit = rvoice->dsp.sample->data24 != NULL;

    if (is_24bit)
    {
        if (looping)
        {
            return func.template operator()<true, true>(rvoice, dsp_buf);
        }
        else
        {
            return func.template operator()<true, false>(rvoice, dsp_buf);
        }
    }
    else
    {
        // This case is most common, thanks to templating it will also become the fastest one
        if (looping)
        {
            return func.template operator()<false, true>(rvoice, dsp_buf);
        }
        else
        {
            return func.template operator()<false, false>(rvoice, dsp_buf);
        }
    }
}

extern "C" int
fluid_rvoice_dsp_silence(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf, int looping)
{
    return dsp_invoker<ProcessSilence>(rvoice, dsp_buf, looping);
}

extern "C" int
fluid_rvoice_dsp_interpolate(fluid_rvoice_t *rvoice, fluid_real_t *FLUID_RESTRICT dsp_buf, int looping)
{
    switch (rvoice->dsp.interp_method)
    {
        case FLUID_INTERP_NONE:
            return dsp_invoker<InterpolateNone>(rvoice, dsp_buf, looping);

        case FLUID_INTERP_LINEAR:
            return dsp_invoker<InterpolateLinear>(rvoice, dsp_buf, looping);

        case FLUID_INTERP_4THORDER:
        default:
            return dsp_invoker<Interpolate4thOrder>(rvoice, dsp_buf, looping);

        case FLUID_INTERP_7THORDER:
            return dsp_invoker<Interpolate7thOrder>(rvoice, dsp_buf, looping);
    }
}