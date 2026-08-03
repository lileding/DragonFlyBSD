/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "virtiod.h"

static void test_valid(void);
static void test_invalid(void);

int
main(void)
{

	test_valid();
	test_invalid();
	puts("PASS: virtiod config");
	return 0;
}

static void
test_valid(void)
{
	struct virtiod_config config;
	FILE *stream;

	stream = tmpfile();
	assert(stream != NULL);
	fputs("# sandbox devices\n"
	    "blk kata0/devices/root path=/var/lib/runv/root.raw,queues=2\n"
	    "net kata0/devices/eth0 tap=tap0,mac=02:df:00:00:00:01\n"
	    "vsock kata0/devices/vsock0 cid=3\n", stream);
	rewind(stream);
	assert(virtiod_config_load(stream, &config) == 0);
	assert(config.mut_count == 3);
	assert(config.own_mut_devices[0].imm_type == VIRTIOD_DEVICE_BLK);
	assert(strcmp(config.own_mut_devices[0].imm_slot_path,
	    "kata0/devices/root") == 0);
	assert(strcmp(config.own_mut_devices[1].imm_mac,
	    "02:df:00:00:00:01") == 0);
	assert(config.own_mut_devices[0].imm_queue_count == 2);
	assert(config.own_mut_devices[1].imm_queue_count == 1);
	assert(config.own_mut_devices[2].imm_type == VIRTIOD_DEVICE_VSOCK);
	assert(config.own_mut_devices[2].imm_guest_cid == 3);
	virtiod_config_fini(&config);
	fclose(stream);
}

static void
test_invalid(void)
{
	static const char *const input[] = {
		"blk /kata0/devices/root path=/tmp/root.raw\n",
		"blk kata0/other/root path=/tmp/root.raw\n",
		"blk kata0/devices/root tap=tap0\n",
		"net kata0/devices/eth0 tap=tap0\n",
		"blk kata0/devices/root path=/tmp/a,path=/tmp/b\n",
		"blk kata0/devices/root path=/tmp/a,queues=0\n",
		"blk kata0/devices/root path=/tmp/a,queues=65\n",
		"net kata0/devices/eth0 tap=tap0,mac=02:df:00:00:00:01,queues=2\n",
		"vsock kata0/devices/vsock0 cid=2\n",
		"vsock kata0/devices/vsock0 cid=3,path=/tmp/x\n",
		"vsock kata0/devices/vsock0 cid=3\nvsock kata0/devices/vsock1 cid=4\n",
		"blk kata0/devices/root path=/tmp/a\nblk kata0/devices/root path=/tmp/b\n",
		"unknown kata0/devices/root path=/tmp/root.raw\n"
	};
	unsigned int i;

	for (i = 0; i < sizeof(input) / sizeof(input[0]); i++) {
		struct virtiod_config config;
		FILE *stream;

		stream = tmpfile();
		assert(stream != NULL);
		fputs(input[i], stream);
		rewind(stream);
		assert(virtiod_config_load(stream, &config) != 0);
		fclose(stream);
	}
}
