#include "force_release_build.h"

#include "define_deegen_common_snippet.h"
#include "bytecode.h"

static uint64_t* DeegenSnippet_GetEndOfCallFrame(uint64_t* stackFrameBase, CodeBlock* cb)
{
    return stackFrameBase + cb->m_stackFrameNumSlots;
}

DEFINE_DEEGEN_COMMON_SNIPPET("GetEndOfCallFrame", DeegenSnippet_GetEndOfCallFrame)
