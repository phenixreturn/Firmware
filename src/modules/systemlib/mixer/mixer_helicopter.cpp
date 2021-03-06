/****************************************************************************
 *
 *   Copyright (c) 2012-2016 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mixer_helicopter.cpp
 *
 * Helicopter mixers.
 */
#include <px4_config.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <math.h>

#include <px4iofirmware/protocol.h>

#include "mixer.h"

#define debug(fmt, args...)	do { } while(0)
//#define debug(fmt, args...)	do { printf("[mixer] " fmt "\n", ##args); } while(0)
//#include <debug.h>
//#define debug(fmt, args...)	lowsyslog(fmt "\n", ##args)


namespace
{

float constrain(float val, float min, float max)
{
	return (val < min) ? min : ((val > max) ? max : val);
}

} // anonymous namespace

HelicopterMixer::HelicopterMixer(ControlCallback control_cb,
				 uintptr_t cb_handle,
				 mixer_heli_s *mixer_info) :
	Mixer(control_cb, cb_handle),
	_mixer_info(*mixer_info)
{

}

HelicopterMixer::~HelicopterMixer()
{
}

HelicopterMixer *
HelicopterMixer::from_text(Mixer::ControlCallback control_cb, uintptr_t cb_handle, const char *buf, unsigned &buflen)
{
	mixer_heli_s mixer_info;
	unsigned swash_plate_servo_count = 0;
	unsigned u[5];
	int s[5];
	int used;

	/* enforce that the mixer ends with space or a new line */
	for (int i = buflen - 1; i >= 0; i--) {
		if (buf[i] == '\0') {
			continue;
		}

		/* require a space or newline at the end of the buffer, fail on printable chars */
		if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r') {
			/* found a line ending or space, so no split symbols / numbers. good. */
			break;

		} else {
			debug("simple parser rejected: No newline / space at end of buf. (#%d/%d: 0x%02x)", i, buflen - 1, buf[i]);
			return nullptr;
		}

	}

	if (sscanf(buf, "H: %u%n", &swash_plate_servo_count, &used) != 1) {
		debug("helicopter parse failed on '%s'", buf);
		return nullptr;
	}

	if (swash_plate_servo_count < 3 || swash_plate_servo_count > 4) {
		debug("only supporting swash plate with 3 or 4 servos");
		return nullptr;
	}

	if (used > (int)buflen) {
		debug("OVERFLOW: helicopter spec used %d of %u", used, buflen);
		return nullptr;
	}

	buf = skipline(buf, buflen);

	if (buf == nullptr) {
		debug("no line ending, line is incomplete");
		return nullptr;
	}

	buf = findtag(buf, buflen, 'T');

	if ((buf == nullptr) || (buflen < 12)) {
		debug("control parser failed finding tag, ret: '%s'", buf);
		return nullptr;
	}

	if (sscanf(buf, "T: %u %u %u %u %u",
		   &u[0], &u[1], &u[2], &u[3], &u[4]) != 5) {
		debug("control parse failed on '%s'", buf);
		return nullptr;
	}

	for (unsigned i = 0; i < HELI_CURVES_NR_POINTS; i++) {
		mixer_info.throttle_curve[i] = ((float) u[i]) / 10000.0f;
	}

	buf = skipline(buf, buflen);

	if (buf == nullptr) {
		debug("no line ending, line is incomplete");
		return nullptr;
	}

	buf = findtag(buf, buflen, 'P');

	if ((buf == nullptr) || (buflen < 12)) {
		debug("control parser failed finding tag, ret: '%s'", buf);
		return nullptr;
	}

	if (sscanf(buf, "P: %d %d %d %d %d",
		   &s[0], &s[1], &s[2], &s[3], &s[4]) != 5) {
		debug("control parse failed on '%s'", buf);
		return nullptr;
	}

	for (unsigned i = 0; i < HELI_CURVES_NR_POINTS; i++) {
		mixer_info.pitch_curve[i] = ((float) s[i]) / 10000.0f;
	}

	buf = skipline(buf, buflen);

	if (buf == nullptr) {
		debug("no line ending, line is incomplete");
		return nullptr;
	}

	mixer_info.control_count = swash_plate_servo_count;

	/* Now loop through the servos */
	for (unsigned i = 0; i < mixer_info.control_count; i++) {

		buf = findtag(buf, buflen, 'S');

		if ((buf == nullptr) || (buflen < 12)) {
			debug("control parser failed finding tag, ret: '%s'", buf);
			return nullptr;
		}

		if (sscanf(buf, "S: %u %u %d %d %d %d",
			   &u[0],
			   &u[1],
			   &s[0],
			   &s[1],
			   &s[2],
			   &s[3]) != 6) {
			debug("control parse failed on '%s'", buf);
			return nullptr;
		}

		mixer_info.servos[i].angle = ((float) u[0]) * M_PI_F / 180.0f;
		mixer_info.servos[i].arm_length = ((float) u[1]) / 10000.0f;
		mixer_info.servos[i].scale = ((float) s[0]) / 10000.0f;
		mixer_info.servos[i].offset = ((float) s[1]) / 10000.0f;
		mixer_info.servos[i].min_output = ((float) s[2]) / 10000.0f;
		mixer_info.servos[i].max_output = ((float) s[3]) / 10000.0f;

		buf = skipline(buf, buflen);

		if (buf == nullptr) {
			debug("no line ending, line is incomplete");
			return nullptr;
		}
	}

	debug("remaining in buf: %d, first char: %c", buflen, buf[0]);

	HelicopterMixer *hm = new HelicopterMixer(
		control_cb,
		cb_handle,
		&mixer_info);

	if (hm != nullptr) {
		debug("loaded heli mixer with %d swash plate input(s)", mixer_info.control_count);

	} else {
		debug("could not allocate memory for mixer");
	}

	return hm;
}

unsigned
HelicopterMixer::mix(float *outputs, unsigned space, uint16_t *status_reg)
{
	/* Find index to use for curves */
	float thrust_cmd = get_control(0, 3);
	int idx = (thrust_cmd / 0.25f);

	/* Make sure idx is in range */
	if (idx < 0) { idx = 0; }

	if (idx > HELI_CURVES_NR_POINTS - 1) { idx = HELI_CURVES_NR_POINTS - 1; }

	/* Local throttle curve gradient and offset */
	float tg = (_mixer_info.throttle_curve[idx + 1] - _mixer_info.throttle_curve[idx]) / 0.25f;
	float to = (_mixer_info.throttle_curve[idx]) - (tg * idx * 0.25f);
	float throttle = constrain((tg * thrust_cmd + to), 0.0f, 1.0f);

	/* Local pitch curve gradient and offset */
	float pg = (_mixer_info.pitch_curve[idx + 1] - _mixer_info.pitch_curve[idx]) / 0.25f;
	float po = (_mixer_info.pitch_curve[idx]) - (pg * idx * 0.25f);
	float collective_pitch = constrain((pg * thrust_cmd + po), -0.5f, 0.5f);

	float roll_cmd = get_control(0, 0);
	float pitch_cmd = get_control(0, 1);

	outputs[0] = throttle;

	for (unsigned i = 0; i < _mixer_info.control_count; i++) {
		outputs[i + 1] = collective_pitch
				 + cosf(_mixer_info.servos[i].angle) * pitch_cmd * _mixer_info.servos[i].arm_length
				 - sinf(_mixer_info.servos[i].angle) * roll_cmd * _mixer_info.servos[i].arm_length;
		outputs[i + 1] *= _mixer_info.servos[i].scale;
		outputs[i + 1] += _mixer_info.servos[i].offset;
		outputs[i + 1] = constrain(outputs[i + 1], _mixer_info.servos[i].min_output, _mixer_info.servos[i].max_output);
	}

	return _mixer_info.control_count + 1;
}

void
HelicopterMixer::groups_required(uint32_t &groups)
{
	/* XXX for now, hardcoded to indexes 0-3 in control group zero */
	groups |= (1 << 0);
}

