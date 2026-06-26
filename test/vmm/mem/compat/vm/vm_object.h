#ifndef VMM_TEST_MEM_COMPAT_VM_VM_OBJECT_H
#define VMM_TEST_MEM_COMPAT_VM_VM_OBJECT_H

#define OBJ_NOSPLIT	0x01

struct vm_object {
	int dummy;
};

static inline void
vm_object_hold(struct vm_object *object)
{
	(void)object;
}

static inline void
vm_object_reference_locked(struct vm_object *object)
{
	(void)object;
}

static inline void
vm_object_reference_quick(struct vm_object *object)
{
	(void)object;
}

static inline void
vm_object_drop(struct vm_object *object)
{
	(void)object;
}

static inline void
vm_object_deallocate(struct vm_object *object)
{
	(void)object;
}

static inline void
vm_object_set_flag(struct vm_object *object, int flag)
{
	(void)object;
	(void)flag;
}

#endif /* VMM_TEST_MEM_COMPAT_VM_VM_OBJECT_H */
