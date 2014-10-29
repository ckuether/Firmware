/****************************************************************************
 *
 *   Copyright (c) 2013, 2014 PX4 Development Team. All rights reserved.
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
 * @file VTOL_att_control_main.cpp
 * Implementation of an attitude controller for VTOL airframes. This module receives data
 * from both the fixed wing- and the multicopter attitude controllers and processes it.
 * It computes the correct actuator controls depending on which mode the vehicle is in (hover,forward-
 * flight or transition). It also publishes the resulting controls on the actuator controls topics.
 *
 * @author Roman Bapst 		<bapstr@ethz.ch>
 * @author Lorenz Meier 	<lm@inf.ethz.ch>
 * @author Thomas Gubler	<thomasgubler@gmail.com>
 *
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <drivers/drv_hrt.h>
#include <arch/board/board.h>
#include <uORB/uORB.h>
#include <uORB/topics/vehicle_attitude_setpoint.h>
#include <uORB/topics/manual_control_setpoint.h>
#include <uORB/topics/actuator_controls.h>
#include <uORB/topics/vehicle_rates_setpoint.h>
#include <uORB/topics/vehicle_attitude.h>
#include <uORB/topics/vehicle_control_mode.h>
#include <uORB/topics/vtol_vehicle_status.h>
#include <uORB/topics/actuator_armed.h>
#include <uORB/topics/parameter_update.h>
#include <systemlib/param/param.h>
#include <systemlib/err.h>
#include <systemlib/perf_counter.h>
#include <systemlib/systemlib.h>
#include <systemlib/circuit_breaker.h>
#include <lib/mathlib/mathlib.h>
#include <lib/geo/geo.h>

#include "drivers/drv_pwm_output.h"
#include <nuttx/fs/nxffs.h>
#include <nuttx/fs/ioctl.h>

#include <nuttx/mtd.h>

#include <fcntl.h>


extern "C" __EXPORT int vtol_att_control_main(int argc, char *argv[]);

class VtolAttitudeControl
{
public:

	VtolAttitudeControl();
	~VtolAttitudeControl();

	int start();	/* start the task and return OK on success */


private:
//******************flags & handlers******************************************************
	bool _task_should_exit;
	int _control_task;		//task handle for VTOL attitude controller

	/* handlers for subscriptions */
	int		_v_att_sub;				//vehicle attitude subscription
	int		_v_att_sp_sub;			//vehicle attitude setpoint subscription
	int		_v_rates_sp_sub;		//vehicle rates setpoint subscription
	int		_v_control_mode_sub;	//vehicle control mode subscription
	int		_params_sub;			//parameter updates subscription
	int		_manual_control_sp_sub;	//manual control setpoint subscription
	int		_armed_sub;				//arming status subscription

	int 	_actuator_inputs_mc;	//topic on which the mc_att_controller publishes actuator inputs
	int 	_actuator_inputs_fw;	//topic on which the fw_att_controller publishes actuator inputs

	//handlers for publishers
	orb_advert_t	_actuators_0_pub;		//input for the mixer (roll,pitch,yaw,thrust)
	orb_advert_t 	_actuators_1_pub;
	orb_advert_t	_vtol_vehicle_status_pub;
//*******************data containers***********************************************************
	struct vehicle_attitude_s			_v_att;				//vehicle attitude
	struct vehicle_attitude_setpoint_s	_v_att_sp;			//vehicle attitude setpoint
	struct vehicle_rates_setpoint_s		_v_rates_sp;		//vehicle rates setpoint
	struct manual_control_setpoint_s	_manual_control_sp; //manual control setpoint
	struct vehicle_control_mode_s		_v_control_mode;	//vehicle control mode
	struct vtol_vehicle_status_s 		_vtol_vehicle_status;
	struct actuator_controls_s			_actuators_out_0;	//actuator controls going to the mc mixer
	struct actuator_controls_s			_actuators_out_1;	//actuator controls going to the fw mixer (used for elevons)
	struct actuator_controls_s			_actuators_mc_in;	//actuator controls from mc_att_control
	struct actuator_controls_s			_actuators_fw_in;	//actuator controls from fw_att_control
	struct actuator_armed_s				_armed;				//actuator arming status

	struct {
		float min_pwm_mc;	//pwm value for idle in mc mode
	} _params;

	struct {
		param_t min_pwm_mc;
	} _params_handles;

	perf_counter_t	_loop_perf;			/**< loop performance counter */

	/* for multicopters it is usual to have a non-zero idle speed of the engines
	 * for fixed wings we want to have an idle speed of zero since we do not want
	 * to waste energy when gliding. */
	bool flag_idle_mc;		//false = "idle is set for fixed wing mode"; true = "idle is set for multicopter mode"

//*****************Member functions***********************************************************************

	void 		task_main();	//main task
	static void	task_main_trampoline(int argc, char *argv[]);	//Shim for calling task_main from task_create.

	void		vehicle_control_mode_poll();	//Check for changes in vehicle control mode.
	void		vehicle_manual_poll();			//Check for changes in manual inputs.
	void		arming_status_poll();			//Check for arming status updates.
	void 		actuator_controls_mc_poll();	//Check for changes in mc_attitude_control output
	void 		actuator_controls_fw_poll();	//Check for changes in fw_attitude_control output
	void 		parameters_update_poll();		//Check if parameters have changed
	int 		parameters_update();			//Update local paraemter cache
	void  		fill_mc_att_control_output();	//write mc_att_control results to actuator message
	void		fill_fw_att_control_output();	//write fw_att_control results to actuator message
	void 		set_idle_fw();
	void 		set_idle_mc();
};

namespace VTOL_att_control
{
VtolAttitudeControl *g_control;
}

/**
* Constructor
*/
VtolAttitudeControl::VtolAttitudeControl() :
	_task_should_exit(false),
	_control_task(-1),

	//init subscription handlers
	_v_att_sub(-1),
	_v_att_sp_sub(-1),
	_v_control_mode_sub(-1),
	_params_sub(-1),
	_manual_control_sp_sub(-1),
	_armed_sub(-1),

	//init publication handlers
	_actuators_0_pub(-1),
	_actuators_1_pub(-1),
	_vtol_vehicle_status_pub(-1),

	_loop_perf(perf_alloc(PC_ELAPSED, "mc_att_control"))
{

	flag_idle_mc = true;	/*assume we always start in mc mode for a VTOL airframe */

	memset(& _vtol_vehicle_status, 0, sizeof(_vtol_vehicle_status));
	_vtol_vehicle_status.vtol_in_rw_mode = true;	/* start vtol in rotary wing mode*/
	memset(&_v_att, 0, sizeof(_v_att));
	memset(&_v_att_sp, 0, sizeof(_v_att_sp));
	memset(&_v_rates_sp, 0, sizeof(_v_rates_sp));
	memset(&_manual_control_sp, 0, sizeof(_manual_control_sp));
	memset(&_v_control_mode, 0, sizeof(_v_control_mode));
	memset(&_vtol_vehicle_status, 0, sizeof(_vtol_vehicle_status));
	memset(&_actuators_out_0, 0, sizeof(_actuators_out_0));
	memset(&_actuators_out_1, 0, sizeof(_actuators_out_1));
	memset(&_actuators_mc_in, 0, sizeof(_actuators_mc_in));
	memset(&_actuators_fw_in, 0, sizeof(_actuators_fw_in));
	memset(&_armed, 0, sizeof(_armed));

	_params_handles.min_pwm_mc = param_find("PWM_MIN");

	/* fetch initial parameter values */
	parameters_update();
}

/**
* Destructor
*/
VtolAttitudeControl::~VtolAttitudeControl()
{
	if (_control_task != -1) {
		/* task wakes up every 100ms or so at the longest */
		_task_should_exit = true;

		/* wait for a second for the task to quit at our request */
		unsigned i = 0;

		do {
			/* wait 20ms */
			usleep(20000);

			/* if we have given up, kill it */
			if (++i > 50) {
				task_delete(_control_task);
				break;
			}
		} while (_control_task != -1);
	}

	VTOL_att_control::g_control = nullptr;
}

/**
* Check for changes in vehicle control mode.
*/
void VtolAttitudeControl::vehicle_control_mode_poll()
{
	bool updated;

	/* Check if vehicle control mode has changed */
	orb_check(_v_control_mode_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_control_mode), _v_control_mode_sub, &_v_control_mode);
	}
}

/**
* Check for changes in manual inputs.
*/
void VtolAttitudeControl::vehicle_manual_poll()
{
	bool updated;

	/* get pilots inputs */
	orb_check(_manual_control_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(manual_control_setpoint), _manual_control_sp_sub, &_manual_control_sp);
	}
}
/**
* Check for arming status updates.
*/
void VtolAttitudeControl::arming_status_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_armed_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(actuator_armed), _armed_sub, &_armed);
	}
}

/**
* Check for inputs from mc attitude controller.
*/
void VtolAttitudeControl::actuator_controls_mc_poll()
{
	bool updated;
	orb_check(_actuator_inputs_mc, &updated);

	if (updated) {
		orb_copy(ORB_ID(actuator_controls_virtual_mc), _actuator_inputs_mc , &_actuators_mc_in);
	}
}

/**
* Check for inputs from fw attitude controller.
*/
void VtolAttitudeControl::actuator_controls_fw_poll()
{
	bool updated;
	orb_check(_actuator_inputs_fw, &updated);

	if (updated) {
		orb_copy(ORB_ID(actuator_controls_virtual_fw), _actuator_inputs_fw , &_actuators_fw_in);
	}
}

/**
* Check for parameter updates.
*/
void
VtolAttitudeControl::parameters_update_poll()
{
	bool updated;

	/* Check if parameters have changed */
	orb_check(_params_sub, &updated);

	if (updated) {
		struct parameter_update_s param_update;
		orb_copy(ORB_ID(parameter_update), _params_sub, &param_update);
		parameters_update();
	}
}

/**
* Update parameters.
*/
int
VtolAttitudeControl::parameters_update()
{
	/* idle pwm */
	float v;
	param_get(_params_handles.min_pwm_mc, &v);
	_params.min_pwm_mc = v;

	return OK;
}

/**
* Prepare message to acutators with data from mc attitude controller.
*/
void VtolAttitudeControl::fill_mc_att_control_output()
{
	_actuators_out_0.control[0] = _actuators_mc_in.control[0];
	_actuators_out_0.control[1] = _actuators_mc_in.control[1];
	_actuators_out_0.control[2] = _actuators_mc_in.control[2];
	_actuators_out_0.control[3] = _actuators_mc_in.control[3];
	//set neutral position for elevons
	_actuators_out_1.control[0] = 0;	//roll elevon
	_actuators_out_1.control[1] = 0;	//pitch elevon
}

/**
* Prepare message to acutators with data from fw attitude controller.
*/
void VtolAttitudeControl::fill_fw_att_control_output()
{
	/*For the first test in fw mode, only use engines for thrust!!!*/
	_actuators_out_0.control[0] = 0;
	_actuators_out_0.control[1] = 0;
	_actuators_out_0.control[2] = 0;
	_actuators_out_0.control[3] = _actuators_fw_in.control[3];
	/*controls for the elevons */
	_actuators_out_1.control[0] = _actuators_fw_in.control[0];	/*roll elevon*/
	_actuators_out_1.control[1] = _actuators_fw_in.control[1];	/*pitch elevon */
}

/**
* Adjust idle speed for fw mode.
*/
void VtolAttitudeControl::set_idle_fw()
{
	int ret;
	char *dev = PWM_OUTPUT_DEVICE_PATH;
	int fd = open(dev, 0);

	if (fd < 0) {err(1, "can't open %s", dev);}

	unsigned pwm_value = PWM_LOWEST_MIN;
	struct pwm_output_values pwm_values;
	memset(&pwm_values, 0, sizeof(pwm_values));

	for (unsigned i = 0; i < 4; i++) {

		pwm_values.values[i] = pwm_value;
		pwm_values.channel_count++;
	}

	ret = ioctl(fd, PWM_SERVO_SET_MIN_PWM, (long unsigned int)&pwm_values);

	if (ret != OK) {errx(ret, "failed setting min values");}

	close(fd);
}

/**
* Adjust idle speed for mc mode.
*/
void VtolAttitudeControl::set_idle_mc()
{
	int ret;
	unsigned servo_count;
	char *dev = PWM_OUTPUT_DEVICE_PATH;
	int fd = open(dev, 0);

	if (fd < 0) {err(1, "can't open %s", dev);}

	ret = ioctl(fd, PWM_SERVO_GET_COUNT, (unsigned long)&servo_count);
	unsigned pwm_value = 1100;
	struct pwm_output_values pwm_values;
	memset(&pwm_values, 0, sizeof(pwm_values));

	for (unsigned i = 0; i < 4; i++) {
		pwm_values.values[i] = pwm_value;
		pwm_values.channel_count++;
	}

	ret = ioctl(fd, PWM_SERVO_SET_MIN_PWM, (long unsigned int)&pwm_values);

	if (ret != OK) {errx(ret, "failed setting min values");}

	close(fd);
}

void
VtolAttitudeControl::task_main_trampoline(int argc, char *argv[])
{
	VTOL_att_control::g_control->task_main();
}

void VtolAttitudeControl::task_main()
{
	warnx("started");
	fflush(stdout);

	/* do subscriptions */
	_v_att_sp_sub          = orb_subscribe(ORB_ID(vehicle_attitude_setpoint));
	_v_rates_sp_sub        = orb_subscribe(ORB_ID(vehicle_rates_setpoint));
	_v_att_sub             = orb_subscribe(ORB_ID(vehicle_attitude));
	_v_control_mode_sub    = orb_subscribe(ORB_ID(vehicle_control_mode));
	_params_sub            = orb_subscribe(ORB_ID(parameter_update));
	_manual_control_sp_sub = orb_subscribe(ORB_ID(manual_control_setpoint));
	_armed_sub             = orb_subscribe(ORB_ID(actuator_armed));

	_actuator_inputs_mc    = orb_subscribe(ORB_ID(actuator_controls_virtual_mc));
	_actuator_inputs_fw    = orb_subscribe(ORB_ID(actuator_controls_virtual_fw));

	parameters_update();  /*initialize parameter cache/*

	/* wakeup source*/
	struct pollfd fds[3];	/*input_mc, input_fw, parameters*/

	fds[0].fd     = _actuator_inputs_mc;
	fds[0].events = POLLIN;
	fds[1].fd     = _actuator_inputs_fw;
	fds[1].events = POLLIN;
	fds[2].fd     = _params_sub;
	fds[2].events = POLLIN;

	while (!_task_should_exit) {
		/*Advertise/Publish vtol vehicle status*/
		if (_vtol_vehicle_status_pub > 0) {
			orb_publish(ORB_ID(vtol_vehicle_status), _vtol_vehicle_status_pub, &_vtol_vehicle_status);

		} else {
			_vtol_vehicle_status.timestamp = hrt_absolute_time();
			_vtol_vehicle_status_pub = orb_advertise(ORB_ID(vtol_vehicle_status), &_vtol_vehicle_status);
		}

		/* wait for up to 100ms for data */
		int pret = poll(&fds[0], (sizeof(fds) / sizeof(fds[0])), 100);


		/* timed out - periodic check for _task_should_exit */
		if (pret == 0) {
			continue;
		}

		/* this is undesirable but not much we can do - might want to flag unhappy status */
		if (pret < 0) {
			warn("poll error %d, %d", pret, errno);
			/* sleep a bit before next try */
			usleep(100000);
			continue;
		}

		if (fds[2].revents & POLLIN) {	//parameters were updated, read them now
			/* read from param to clear updated flag */
			struct parameter_update_s update;
			orb_copy(ORB_ID(parameter_update), _params_sub, &update);

			/* update parameters from storage */
			parameters_update();
		}

		vehicle_control_mode_poll();	//Check for changes in vehicle control mode.
		vehicle_manual_poll();			//Check for changes in manual inputs.
		arming_status_poll();			//Check for arming status updates.
		actuator_controls_mc_poll();	//Check for changes in mc_attitude_control output
		actuator_controls_fw_poll();	//Check for changes in fw_attitude_control output
		parameters_update_poll();

		if (_manual_control_sp.aux1 <= 0.0f) {		/* vehicle is in mc mode */
			_vtol_vehicle_status.vtol_in_rw_mode = true;

			if (!flag_idle_mc) {	/* we want to adjust idle speed for mc mode */
				set_idle_mc();
				flag_idle_mc = true;
			}

			/* got data from mc_att_controller */
			if (fds[0].revents & POLLIN) {
				vehicle_manual_poll();	/* update remote input */
				orb_copy(ORB_ID(actuator_controls_virtual_mc), _actuator_inputs_mc, &_actuators_mc_in);

				fill_mc_att_control_output();

				if (_actuators_0_pub > 0) {
					orb_publish(ORB_ID(actuator_controls_0), _actuators_0_pub, &_actuators_out_0);

				} else {
					_actuators_0_pub = orb_advertise(ORB_ID(actuator_controls_0), &_actuators_out_0);
				}

				if (_actuators_1_pub > 0) {
					orb_publish(ORB_ID(actuator_controls_1), _actuators_1_pub, &_actuators_out_1);

				} else {
					_actuators_1_pub = orb_advertise(ORB_ID(actuator_controls_1), &_actuators_out_1);
				}
			}
		}

		if (_manual_control_sp.aux1 >= 0.0f) {			/* vehicle is in fw mode */
			_vtol_vehicle_status.vtol_in_rw_mode = false;

			if (flag_idle_mc) {	/* we want to adjust idle speed for fixed wing mode */
				set_idle_fw();
				flag_idle_mc = false;
			}

			if (fds[1].revents & POLLIN) {		/* got data from fw_att_controller */
				orb_copy(ORB_ID(actuator_controls_virtual_fw), _actuator_inputs_fw, &_actuators_fw_in);
				vehicle_manual_poll();	//update remote input

				fill_fw_att_control_output();

				if (_actuators_0_pub > 0) {
					orb_publish(ORB_ID(actuator_controls_0), _actuators_0_pub, &_actuators_out_0);

				} else {
					_actuators_0_pub = orb_advertise(ORB_ID(actuator_controls_0), &_actuators_out_0);
				}

				if (_actuators_1_pub > 0) {
					orb_publish(ORB_ID(actuator_controls_1), _actuators_1_pub, &_actuators_out_1);

				} else {
					_actuators_1_pub = orb_advertise(ORB_ID(actuator_controls_1), &_actuators_out_1);
				}
			}
		}
	}

	warnx("exit");
	_control_task = -1;
	_exit(0);
}

int
VtolAttitudeControl::start()
{
	ASSERT(_control_task == -1);

	/* start the task */
	_control_task = task_spawn_cmd("vtol_att_control",
				       SCHED_DEFAULT,
				       SCHED_PRIORITY_MAX - 10,
				       2048,
				       (main_t)&VtolAttitudeControl::task_main_trampoline,
				       nullptr);

	if (_control_task < 0) {
		warn("task start failed");
		return -errno;
	}

	return OK;
}


int vtol_att_control_main(int argc, char *argv[])
{
	if (argc < 1) {
		errx(1, "usage: vtol_att_control {start|stop|status}");
	}

	if (!strcmp(argv[1], "start")) {

		if (VTOL_att_control::g_control != nullptr) {
			errx(1, "already running");
		}

		VTOL_att_control::g_control = new VtolAttitudeControl;

		if (VTOL_att_control::g_control == nullptr) {
			errx(1, "alloc failed");
		}

		if (OK != VTOL_att_control::g_control->start()) {
			delete VTOL_att_control::g_control;
			VTOL_att_control::g_control = nullptr;
			err(1, "start failed");
		}

		exit(0);
	}

	if (!strcmp(argv[1], "stop")) {
		if (VTOL_att_control::g_control == nullptr) {
			errx(1, "not running");
		}

		delete VTOL_att_control::g_control;
		VTOL_att_control::g_control = nullptr;
		exit(0);
	}

	if (!strcmp(argv[1], "status")) {
		if (VTOL_att_control::g_control) {
			errx(0, "running");

		} else {
			errx(1, "not running");
		}
	}

	warnx("unrecognized command");
	return 1;
}