/**
 * @file led_patterns.h
 * @brief The showcase pattern list, for leds.c to browse.
 */

#pragma once

#include "tapp_api.h"

typedef struct {
    const char* name;
    led_pattern_t* pattern;
} led_demo_t;

extern const led_demo_t led_demos[];
extern const uint16_t led_demo_count;
