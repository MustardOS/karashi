#ifndef KARASHI_SCHEDULER_H
#define KARASHI_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

enum karashi_device {
	KARASHI_DEVICE_MAIN_CPU = 0,
	KARASHI_DEVICE_SOUND_CPU,
	KARASHI_DEVICE_VIDEO,
	KARASHI_DEVICE_AUDIO,
	KARASHI_DEVICE_MEDIA,
	KARASHI_DEVICE_COUNT
};

struct karashi_scheduler {
	uint64_t now;
	uint64_t deadline[KARASHI_DEVICE_COUNT];
};

void karashi_scheduler_reset(struct karashi_scheduler *scheduler);
bool karashi_scheduler_schedule(struct karashi_scheduler *scheduler, enum karashi_device device, uint64_t deadline);
bool karashi_scheduler_cancel(struct karashi_scheduler *scheduler, enum karashi_device device);
bool karashi_scheduler_next(
	struct karashi_scheduler *scheduler, enum karashi_device *device, uint64_t *deadline
);
uint64_t karashi_scheduler_now(const struct karashi_scheduler *scheduler);

#endif
