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


static void *minitrace_flush_loop(void *arg) {
  mtr_otlp_grpcio_cfg gcfg = mtr_create_def_otlp_grpcio_cfg();
  mtr_otlp_exp_cfg ecfg = mtr_create_def_otlp_exp_cfg();
  mtr_coll_cfg cfg = mtr_create_def_coll_cfg();
  mtr_coll_cfg cfg_2 = mtr_set_report_max_spans(cfg, 200);
  mtr_otel_rptr rptr = mtr_create_otel_rptr(ecfg, gcfg);
  mtr_set_otel_rptr(rptr, cfg_2);

  while (1) {
    mtr_flush();
    sleep(5);
  }
  return NULL;
}


static inline void minitrace_init() {
//   LogEvent(COMPONENT_INIT, "Init minitrace");
  static bool initialised;
  if (initialised)
    return;
  fprintf(stderr, "Init minitrace\n");

  pthread_t t1;
  pthread_create(&t1, NULL, minitrace_flush_loop, NULL);
  initialised = true;
}

#ifdef __cplusplus
}
#endif

#endif   /* GANESHA_MINITRACE_H */
