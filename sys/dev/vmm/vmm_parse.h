/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Text scanners shared by the config value objects (vcpu/mem/loader).
 * No kernel or VFS dependencies -- part of the reusable vmm_ core.
 *
 * Types (uint64_t/size_t) come from the includer (<sys/types.h> in the kernel,
 * <stdint.h>/<stddef.h> on the host).
 */
#ifndef VMM_PARSE_H
#define VMM_PARSE_H

int		vmm_is_ws(char c);
/* Trim leading/trailing whitespace; returns the start, sets *outlen. */
const char     *vmm_trim(const char *b, size_t len, size_t *outlen);
/* Parse a non-empty decimal with overflow check; 1 on success, 0 on reject. */
int		vmm_parse_decimal(const char *b, size_t len, uint64_t *out);
/* Write a decimal + '\n'; returns bytes written (0 if it does not fit). */
size_t		vmm_write_decimal(uint64_t v, char *out, size_t cap);

#endif /* VMM_PARSE_H */
