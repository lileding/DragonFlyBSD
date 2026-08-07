/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core: one loader process run.
 *
 * The machine object owns the configured path as plain text.  struct
 * vmm_loader is the short-lived execution object used by vmm_machine_command_start().
 */
#ifndef VMM_LOADER_H
#define VMM_LOADER_H

#include <sys/types.h>

#include "vmm_domain.h"

#define VMM_LOADER_MAX	256

#define VMM_LOADER_INITING	-3
#define VMM_LOADER_PAUSED	-2
#define VMM_LOADER_RUNNING	-1
#define VMM_LOADER_OK		0
#define VMM_LOADER_FAILED	 1

struct file;
struct ucred;
struct vm_object;
struct vmm_launch;

struct vmm_loader {
	const char	*imm_path;
	pid_t		 imm_pid;
	struct vmm_domain_proc_handler own_handler;
	struct file	*own_mut_mem_fp;
	struct file	*own_mut_manifest_fp;
	struct vm_object *own_mut_manifest_object;
	uint64_t	 imm_mem_size;
	int		 atomic_mut_state;
	/*
	 * Written by the at_exit callback before atomic_mut_state is published
	 * as OK/FAILED.  Read only after the terminal state is observed.
	 */
	int		 mut_exit_code;
};

int	vmm_loader_path_parse(char *path, size_t *len, const char *buf,
	    size_t buflen);
size_t	vmm_loader_path_format(const char *path, size_t len, char *out,
	    size_t cap);
int	vmm_loader_path_is_set(size_t len);
int	vmm_loader_mmap_active(void);

int	vmm_loader_init(struct vmm_loader *loader, const char *path,
	    struct ucred *cred);
int	vmm_loader_install(struct vmm_loader *loader,
	    struct vm_object *mem_object, uint64_t mem_size);
int	vmm_loader_resume(struct vmm_loader *loader);
int	vmm_loader_wait(struct vmm_loader *loader);
int	vmm_loader_manifest_load(struct vmm_loader *loader,
	    struct vmm_launch *launch);
void	vmm_loader_fini(struct vmm_loader *loader);

#endif /* VMM_LOADER_H */
