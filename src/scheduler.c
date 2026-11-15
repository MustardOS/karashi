#include "scheduler.h"

#include <limits.h>
#include <stddef.h>

static int device_valid(enum karashi_device device) {
	return device >= KARASHI_DEVICE_MAIN_CPU && device < KARASHI_DEVICE_COUNT;
}

void karashi_scheduler_reset(struct karashi_scheduler *scheduler) {
	size_t i;

	if (!scheduler) return;
	scheduler->now = 0;
	for (i = 0; i < KARASHI_DEVICE_COUNT; i++) scheduler->deadline[i] = UINT64_MAX;
}

bool karashi_scheduler_schedule(struct karashi_scheduler *scheduler, enum karashi_device device, uint64_t deadline) {
	if (!scheduler || !device_valid(device) || deadline < scheduler->now || deadline == UINT64_MAX) return false;
	scheduler->deadline[device] = deadline;
	return true;
}

bool karashi_scheduler_cancel(struct karashi_scheduler *scheduler, enum karashi_device device) {
	if (!scheduler || !device_valid(device)) return false;
	scheduler->deadline[device] = UINT64_MAX;
	return true;
}

bool karashi_scheduler_next(
	struct karashi_scheduler *scheduler, enum karashi_device *device, uint64_t *deadline
) {
	enum karashi_device next_device = KARASHI_DEVICE_COUNT;
	uint64_t next_deadline = UINT64_MAX;
	int i;

	if (!scheduler) return false;
	for (i = 0; i < KARASHI_DEVICE_COUNT; i++) {
		if (scheduler->deadline[i] < next_deadline) {
			next_deadline = scheduler->deadline[i];
			next_device = (enum karashi_device) i;
		}
	}

	if (next_device == KARASHI_DEVICE_COUNT) return false;
	scheduler->now = next_deadline;
	scheduler->deadline[next_device] = UINT64_MAX;
	if (device) *device = next_device;
	if (deadline) *deadline = next_deadline;
	return true;
}

uint64_t karashi_scheduler_now(const struct karashi_scheduler *scheduler) {
	return scheduler ? scheduler->now : 0;
}
