/* sampler.h -- statistical CPU profiler for long engine steps.
 * MIT license; see LICENSE. */

#ifndef __SAMPLER_H__
#define __SAMPLER_H__

#include <switch.h>

void sampler_start(Thread *game_thread);
void sampler_step_begin(void);
void sampler_step_end(int frame, unsigned long long step_ms);

#endif
