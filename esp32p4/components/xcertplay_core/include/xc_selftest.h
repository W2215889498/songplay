#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Run known-answer self tests for the crypto and TLV8 layers. */
bool xc_selftest_run(void);

#ifdef __cplusplus
}
#endif
