/*
 * keys.h
 * Copyright (C) 2021 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */


#pragma once
#include "data-types.h"
#include "glfw-wrapper.h"
#include <limits.h>

// Room for the encoding of any key event apart from its text. The longest is the CSI u form
// "\x1b[" key ":" shifted ":" alternate ";" mods ":" action trailer, where the three keys are uint32 (at most 10 digits
// each), mods at most 3 digits and the action 1 digit: 42 bytes with the NUL. Legacy encodings are shorter still.
#define KEY_BUFFER_SIZE 128
// An event's text can be embedded in its encoding (the report associated text flag) as one number per code point,
// each written as a separator and at most 10 digits, so the text adds at most this many bytes per byte of UTF-8.
#define KEY_TEXT_ENCODING_BYTES_PER_BYTE 11
#define SEND_TEXT_TO_CHILD INT_MIN
#define debug debug_input

// The size of the output buffer encode_glfw_key_event() needs for e, or 0 if it would not fit in memory
size_t key_encoding_buffer_size(const GLFWkeyevent *e);
// Writes the encoding of e to output, which must hold at least key_encoding_buffer_size(e) bytes (KEY_BUFFER_SIZE
// suffices for an event without text). Returns its length, SEND_TEXT_TO_CHILD, or <= 0 if nothing is to be sent.
ssize_t encode_glfw_key_event(const GLFWkeyevent *e, const bool cursor_key_mode, const unsigned flags, char *output, size_t output_size);

bool is_modifier_key(const uint32_t key);
