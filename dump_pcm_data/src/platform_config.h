#ifndef DUMP_PCM_PLATFORM_CONFIG_H
#define DUMP_PCM_PLATFORM_CONFIG_H

#ifdef DUMP_PCM_PC
#include "platform_config_pc.h"
#else
#include "platform_config_default.h"
#endif

#define FILE_SIZE_LIMIT (1024UL * 1024UL * 1024UL)

#endif
