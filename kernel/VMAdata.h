#ifndef _VMADATA_H_
#define _VMADATA_H_

enum VMAstate {VMA_UNUSED, VMA_R, VMA_RW};

struct VMAdata{
	enum VMAstate state;
	int shared;
	uint64 init;
	uint64 size;
	uint64 file_init;
	struct file * f;
};

#endif // _VMADATA_H_
