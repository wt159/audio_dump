#define DUMP_PCM_INTERNAL_DEBUG_API 1
#include "../include/dump_pcm_data.h"

int main(void) {
    dump_status_t status = {0};
    return dump_pcm_data_get_status(&status);
}
