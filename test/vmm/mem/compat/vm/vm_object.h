#ifndef VMM_TEST_MEM_COMPAT_VM_VM_OBJECT_H
#define VMM_TEST_MEM_COMPAT_VM_VM_OBJECT_H

#include <stdlib.h>

#define OBJ_NOSPLIT	0x01

struct vm_object {
	int refs;
	int flags;
};

extern int vmm_test_vm_object_free_count;

static inline void
vm_object_hold(struct vm_object *object)
{
	(void)object;
}

static inline void
vm_object_reference_locked(struct vm_object *object)
{
	if (object != NULL)
		object->refs++;
}

static inline void
vm_object_reference_quick(struct vm_object *object)
{
	if (object != NULL)
		object->refs++;
}

static inline void
vm_object_drop(struct vm_object *object)
{
	(void)object;
}

static inline void
vm_object_deallocate(struct vm_object *object)
{
	if (object != NULL && --object->refs == 0) {
		vmm_test_vm_object_free_count++;
		free(object);
	}
}

static inline void
vm_object_set_flag(struct vm_object *object, int flag)
{
	if (object != NULL)
		object->flags |= flag;
}

#endif /* VMM_TEST_MEM_COMPAT_VM_VM_OBJECT_H */
