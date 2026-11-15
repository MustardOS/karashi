#include "scheduler.h"

#include <inttypes.h>
#include <stdio.h>

static int failures;

static void expect_true(const char *name, int value) {
	if (value) return;
	fprintf(stderr, "%s: expected true\n", name);
	failures++;
}

static void expect_false(const char *name, int value) {
	if (!value) return;
	fprintf(stderr, "%s: expected false\n", name);
	failures++;
}

static void expect_u64(const char *name, uint64_t actual, uint64_t expected) {
	if (actual == expected) return;
	fprintf(stderr, "%s: expected %" PRIu64 ", got %" PRIu64 "\n", name, expected, actual);
	failures++;
}

static void expect_device(const char *name, enum karashi_device actual, enum karashi_device expected) {
	if (actual == expected) return;
	fprintf(stderr, "%s: expected device %d, got %d\n", name, expected, actual);
	failures++;
}

int main(void) {
	struct karashi_scheduler scheduler;
	enum karashi_device device;
	uint64_t deadline;

	karashi_scheduler_reset(&scheduler);
	expect_u64("reset time", karashi_scheduler_now(&scheduler), 0);
	expect_false("empty", karashi_scheduler_next(&scheduler, &device, &deadline));

	expect_true("audio schedule", karashi_scheduler_schedule(&scheduler, KARASHI_DEVICE_AUDIO, 20));
	expect_true("video schedule", karashi_scheduler_schedule(&scheduler, KARASHI_DEVICE_VIDEO, 10));
	expect_true("main schedule", karashi_scheduler_schedule(&scheduler, KARASHI_DEVICE_MAIN_CPU, 10));

	expect_true("first pop", karashi_scheduler_next(&scheduler, &device, &deadline));
	expect_device("stable tie", device, KARASHI_DEVICE_MAIN_CPU);
	expect_u64("first deadline", deadline, 10);

	expect_true("second pop", karashi_scheduler_next(&scheduler, &device, &deadline));
	expect_device("second device", device, KARASHI_DEVICE_VIDEO);
	expect_u64("second deadline", deadline, 10);
	expect_false("past schedule", karashi_scheduler_schedule(&scheduler, KARASHI_DEVICE_SOUND_CPU, 9));

	expect_true("cancel", karashi_scheduler_cancel(&scheduler, KARASHI_DEVICE_AUDIO));
	expect_false("cancelled empty", karashi_scheduler_next(&scheduler, &device, &deadline));

	if (failures) return 1;
	puts("scheduler: pass");
	return 0;
}
