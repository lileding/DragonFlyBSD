/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <assert.h>
#include <stdio.h>

#include <sys/vmm_vsock_abi.h>

int
main(void)
{
	struct vmm_vsock_abi_request request;

	assert(VMM_VSOCK_HOST_CID == 2);
	assert(sizeof(request) == 40);
	assert(VMM_VSOCK_ABI_MSG_CONNECT != VMM_VSOCK_ABI_MSG_DEVICE_DOWN);
	puts("PASS: virtiod vsock ABI");
	return 0;
}
