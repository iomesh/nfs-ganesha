#include <minitrace_c/minitrace_c.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

extern bool minitrace_reinit;
extern char *otlp_endpoint;

static void *minitrace_flush_loop(void *arg) {
  // mtr_otlp_grpcio_cfg gcfg = mtr_create_def_otlp_grpcio_cfg();
  // mtr_otlp_exp_cfg ecfg = mtr_create_def_otlp_exp_cfg();
  // mtr_coll_cfg cfg = mtr_create_def_coll_cfg();
  // mtr_coll_cfg cfg_2 = mtr_set_report_max_spans(cfg, 200);
  // mtr_otel_rptr rptr = mtr_create_otel_rptr(ecfg, gcfg);
  // mtr_set_otel_rptr(rptr, cfg_2);

  while (1) {
    mtr_flush();
    sleep(5);
    if (minitrace_reinit) {
      if (otlp_endpoint) {
        setenv("OTEL_EXPORTER_OTLP_ENDPOINT", otlp_endpoint, 1);
      }
      mtr_otlp_grpcio_cfg gcfg = mtr_create_def_otlp_grpcio_cfg();
      mtr_otlp_exp_cfg ecfg = mtr_create_def_otlp_exp_cfg();
      mtr_coll_cfg cfg = mtr_create_def_coll_cfg();
      mtr_coll_cfg cfg_2 = mtr_set_report_max_spans(cfg, 200);
      mtr_otel_rptr rptr = mtr_create_otel_rptr(ecfg, gcfg);
      mtr_set_otel_rptr(rptr, cfg_2);
      minitrace_reinit = false;
    }
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
