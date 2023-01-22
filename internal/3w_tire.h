//
// Created by zr on 23-1-21.
//
#ifndef COMPRESS_3W_TIRE_H
#define COMPRESS_3W_TIRE_H
#include <sys/types.h>
#include "str.h"

#define TIRE_VALUE_TYPE u_int16_t

struct comp_tire_node_s
{
    u_char c;
    struct comp_tire_node_s* left;
    struct comp_tire_node_s* mid;
    struct comp_tire_node_s* right;
    TIRE_VALUE_TYPE value;
};

typedef struct comp_tire_node_s comp_tire_t;

comp_tire_t* comp_tire_get(comp_tire_t*, comp_str_t);
comp_tire_t* comp_tire_put(comp_tire_t*, comp_str_t, TIRE_VALUE_TYPE);
void comp_tire_free(comp_tire_t*);

#endif //COMPRESS_3W_TIRE_H
