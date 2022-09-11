#pragma once

#include "common.h"

#define DEEGEN_COMMON_SNIPPET_NAME_VARNAME x_deegen_common_snippet_name
#define DEEGEN_COMMON_SNIPPET_TARGET_VARNAME x_deegen_common_snippet_target
#define DEFINE_DEEGEN_COMMON_SNIPPET(name, ...)                                                             \
    __attribute__((__used__)) inline constexpr const char* DEEGEN_COMMON_SNIPPET_NAME_VARNAME = name;       \
    __attribute__((__used__)) inline constexpr auto DEEGEN_COMMON_SNIPPET_TARGET_VARNAME = __VA_ARGS__;

constexpr const char* x_deegen_common_snippet_function_name_prefix = "__DeegenImpl_CommonSnippetLib_";
