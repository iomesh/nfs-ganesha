#ifndef GANESHA_MINITRACE_H
#define GANESHA_MINITRACE_H

#include <minitrace_c/minitrace_c.h>
#include "log.h"
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

extern double minitrace_sample_ratio;

int read_minitrace_config(config_file_t in_config, struct config_error_type *err_type);

// Wrapper of mtr_create_span_ctx_loc
mtr_span_ctx gsh_mtr_create_span_ctx_loc(void);

#ifdef __cplusplus
}
#endif

#endif   /* GANESHA_MINITRACE_H */
