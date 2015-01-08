/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <new>

#include <linux/input.h>

#include <cutils/atomic.h>
#include <cutils/log.h>

#include <hardware/sensors.h>
#include <hardware/mot_sensorhub_stml0xx.h>

#include "sensors.h"
#include "SensorBase.h"
#include "AkmSensor.h"
#include "sensorhub_hal.h"

/*****************************************************************************/

struct sensors_poll_context_t {
	sensors_poll_device_1_t device; // must be first

	sensors_poll_context_t();
	~sensors_poll_context_t();
	int activate(int handle, int enabled);
	int setDelay(int handle, int64_t ns);
	int pollEvents(sensors_event_t* data, int count);
	int batch(int handle, int flags, int64_t ns, int64_t timeout);
	int flush(int handle);

private:
	enum {
		accelgyromag = 0,
		akm          = 1,
		numSensorDrivers,
		numFds,
	};

	static const size_t wake = numFds - 1;
	static const char WAKE_MESSAGE = 'W';
	struct pollfd mPollFds[numFds];
	int mWritePipeFd;
	SensorBase* mSensors[numSensorDrivers];

	int handleToDriver(int handle);
};

/*****************************************************************************/

sensors_poll_context_t::sensors_poll_context_t()
{
	mSensors[accelgyromag] = new(std::nothrow) HubSensor();
	if (mSensors[accelgyromag]) {
		mPollFds[accelgyromag].fd = mSensors[accelgyromag]->getFd();
		mPollFds[accelgyromag].events = POLLIN;
		mPollFds[accelgyromag].revents = 0;
	} else {
		ALOGE("out of memory: new failed for HubSensor");
	}

	mSensors[akm] = new(std::nothrow) AkmSensor();
	if (mSensors[akm]) {
		mPollFds[akm].fd = mSensors[akm]->getFd();
		mPollFds[akm].events = POLLIN;
		mPollFds[akm].revents = 0;
	} else {
		ALOGE("out of memory: new failed for AkmSensor");
	}

	int wakeFds[2];
	int result = pipe(wakeFds);
	ALOGE_IF(result<0, "error creating wake pipe (%s)", strerror(errno));
	fcntl(wakeFds[0], F_SETFL, O_NONBLOCK);
	fcntl(wakeFds[1], F_SETFL, O_NONBLOCK);
	mWritePipeFd = wakeFds[1];

	mPollFds[wake].fd = wakeFds[0];
	mPollFds[wake].events = POLLIN;
	mPollFds[wake].revents = 0;
}

sensors_poll_context_t::~sensors_poll_context_t() {
	for (int i=0 ; i<numSensorDrivers ; i++) {
		delete mSensors[i];
	}
	close(mPollFds[wake].fd);
	close(mWritePipeFd);
}

int sensors_poll_context_t::handleToDriver(int handle) {
	switch (handle) {
		case ID_A:
		case ID_L:
		case ID_DR:
		case ID_P:
		case ID_FU:
		case ID_FD:
		case ID_S:
		case ID_CA:
		case ID_A2:
			return accelgyromag;
		case ID_M:
			return akm;
	}
	return -EINVAL;
}

int sensors_poll_context_t::activate(int handle, int enabled) {
	int drv = handleToDriver(handle);
	int err = 0;

	if (drv < 0)
		return -EINVAL;

	err = mSensors[drv]->setEnable(handle, enabled);

	if ((handle == ID_M) && enabled && !err) {
		const char wakeMessage(WAKE_MESSAGE);
		int result = write(mWritePipeFd, &wakeMessage, 1);
		ALOGE_IF(result<0, "error sending wake message (%s)", strerror(errno));
	}

	return err;
}

int sensors_poll_context_t::setDelay(int handle, int64_t ns) {
	int drv = handleToDriver(handle);
	int err = 0;

	if (drv < 0)
		return -EINVAL;

	err = mSensors[drv]->setDelay(handle, ns);
	
	return err;
}

int sensors_poll_context_t::pollEvents(sensors_event_t* data, int count)
{
	int nbEvents = 0;
	int n = 0;
	int ret;
	int err;

	while (true) {
		ret = poll(mPollFds, numFds, nbEvents ? 0 : -1);
		err = errno;
		// Success
		if (ret >= 0)
			break;
		ALOGE("poll() failed (%s)", strerror(err));
		// EINTR is OK
		if (err == EINTR)
			continue;
		else
			return -err;
	}

	for (int i=0 ; count && i<numSensorDrivers ; i++) {
		SensorBase* const sensor(mSensors[i]);
		if ((mPollFds[i].revents & POLLIN) || (sensor->hasPendingEvents())) {
			int nb = sensor->readEvents(data, count);
			count -= nb;
			nbEvents += nb;
			data += nb;
			mPollFds[i].revents = 0;
		}
	}

	if (mPollFds[wake].revents & POLLIN) {
		char msg;
		int result = read(mPollFds[wake].fd, &msg, 1);
		ALOGE_IF(result<0,
			"error reading from wake pipe (%s)",
			strerror(errno));
		ALOGE_IF(msg != WAKE_MESSAGE,
			"unknown message on wake queue (0x%02x)",
			int(msg));
		mPollFds[wake].revents = 0;
	}

	return nbEvents;
}

int sensors_poll_context_t::batch(int handle, int flags, int64_t ns, int64_t timeout)
{
	(void)flags;
	(void)timeout;
	return setDelay(handle, ns);
}

int sensors_poll_context_t::flush(int handle)
{
	return mSensors[accelgyromag]->flush(handle);
}

/*****************************************************************************/

static int poll__close(struct hw_device_t *dev)
{
	sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
	if (ctx) {
		delete ctx;
	}
	return 0;
}

static int poll__activate(struct sensors_poll_device_t *dev,
	int handle, int enabled) {
	sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
	return ctx->activate(handle, enabled);
}

static int poll__setDelay(struct sensors_poll_device_t *dev,
	int handle, int64_t ns) {
	sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
	return ctx->setDelay(handle, ns);
}

static int poll__poll(struct sensors_poll_device_t *dev,
	sensors_event_t* data, int count) {
	sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
	return ctx->pollEvents(data, count);
}

static int poll__batch(sensors_poll_device_1_t *dev,
		int handle, int flags, int64_t ns, int64_t timeout) {
	sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
	return ctx->batch(handle, flags, ns, timeout);
}

static int poll__flush(sensors_poll_device_1_t *dev,
		int handle) {
	sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
	return ctx->flush(handle);
}

/*****************************************************************************/

/** Open a new instance of a sensor device using name */
int init_sensors(hw_module_t const* module, hw_device_t** device)
{
	int status = -EINVAL;

	sensors_poll_context_t *dev = new(std::nothrow) sensors_poll_context_t();
	if (dev) {
		memset(&dev->device, 0, sizeof(sensors_poll_device_1_t));

		dev->device.common.tag      = HARDWARE_DEVICE_TAG;
		dev->device.common.version  = SENSORS_DEVICE_API_VERSION_1_3;
		dev->device.common.module   = const_cast<hw_module_t*>(module);
		dev->device.common.close    = poll__close;
		dev->device.activate        = poll__activate;
		dev->device.setDelay        = poll__setDelay;
		dev->device.poll            = poll__poll;
		dev->device.batch           = poll__batch;
		dev->device.flush           = poll__flush;

		*device = &dev->device.common;
		status = 0;
	} else {
		ALOGE("out of memory: new failed for sensors_poll_context_t");
	}

	return status;
}