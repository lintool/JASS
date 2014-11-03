/*
	CI.H
	----
*/

#ifndef CI_H_
#define CI_H_

#include <stdint.h>

/*
	class CI_VOCAB
	--------------
*/
class CI_vocab
{
public:
	const char *term;
	void (*method)(uint16_t *accumulators);
	uint64_t cf;
	uint64_t df;
};

#endif
