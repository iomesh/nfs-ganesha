#include <minitrace_c/minitrace_c.h>

extern "C" {

mtr_span_ctx gsh_mtr_create_span_ctx_loc(void) {
    return mtr_create_span_ctx_loc();
}

}  // extern "C"
