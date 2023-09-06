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

static void *minitrace_flush_loop(void *arg) {
  mtr_otlp_grpcio_cfg gcfg = mtr_create_def_otlp_grpcio_cfg();
  mtr_otlp_exp_cfg ecfg = mtr_create_def_otlp_exp_cfg();
  mtr_coll_cfg cfg = mtr_create_def_coll_cfg();
  mtr_otel_rptr rptr = mtr_create_otel_rptr(ecfg, gcfg);
  mtr_set_otel_rptr(rptr, cfg);

  while (1) {
    mtr_flush();
    sleep(5);
  }
  return NULL;
}

static void *minitrace_gen_loop(void *arg) {
  while (1) {
    sleep(10);
    mtr_span_ctx p = mtr_create_rand_span_ctx();
    mtr_span r = mtr_create_root_span("test-span-at-minitrace-init", p);
    mtr_loc_par_guar g = mtr_set_loc_par_to_span(&r);
    mtr_destroy_loc_par_guar(g);
    mtr_destroy_span(r);
  } 
  return NULL;
}

static inline void minitrace_init() {
//   LogEvent(COMPONENT_INIT, "Init minitrace");
  static bool initialised;
  if (initialised)
    return;
  fprintf(stderr, "Init minitrace");

  pthread_t t1, t2;
  pthread_create(&t1, NULL, minitrace_flush_loop, NULL);
  pthread_create(&t2, NULL, minitrace_gen_loop, NULL);
//   mtr_destroy_otel_rptr(rptr);
  initialised = true;
}

#ifdef __cplusplus
}
#endif

#endif   /* GANESHA_MINITRACE_H */
