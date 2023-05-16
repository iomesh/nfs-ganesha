#ifndef _RECOVERY_SFS_H
#define _RECOVERY_SFS_H
#include <stdio.h>
#include "gsh_refstr.h"

struct sfs_cluster_parameter {
	uint32_t sessionid;
};

extern struct sfs_cluster_parameter sfs_cluster_param;

#endif	/* _RECOVERY_SFS_H */
