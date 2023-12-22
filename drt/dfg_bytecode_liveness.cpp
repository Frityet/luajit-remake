#include "dfg_bytecode_liveness.h"
#include "dfg_control_flow_and_upvalue_analysis.h"
#include "bytecode_builder.h"

namespace dfg {

// Takes two bitvectors of equal length as input.
// This function does the following:
// (1) Assert that 'copyFrom' is a superset of 'bv'
// (2) Check if 'copyFrom' and 'bv' are different
// (3) Set bv = copyFrom
//
// Returns true if 'copyFrom' and 'bv' are different
//
static bool WARN_UNUSED UpdateBitVectorAfterMonotonicPropagation(TempBitVector& bv /*inout*/, const TempBitVector& copyFrom)
{
    TestAssert(bv.m_length == copyFrom.m_length);
    TestAssert(copyFrom.m_data.get() != bv.m_data.get());
    size_t bvAllocLength = bv.GetAllocLength();
    ConstRestrictPtr<uint64_t> copyFromData = copyFrom.m_data.get();
    RestrictPtr<uint64_t> bvData = bv.m_data.get();
    bool changed = false;
    for (size_t i = 0; i < bvAllocLength; i++)
    {
        TestAssert((bvData[i] & copyFromData[i]) == bvData[i]);
        changed |= (bvData[i] != copyFromData[i]);
        bvData[i] = copyFromData[i];
    }
    return changed;
}

struct BytecodeLivenessBBInfo
{
    size_t m_numBytecodesInBB;
    size_t m_firstBytecodeIndex;
    // The offset of each bytecode in REVERSE order
    //
    uint32_t* m_bytecodeOffset;
    // The uses and defs of the k-th bytecode in REVERSE order can be parsed as the following:
    // Defs: [m_infoIndex[k*2-1], m_infoIndex[k*2]) of m_info where m_infoIndex[-1] is assumed to be 0
    // Uses: [m_infoIndex[k*2], m_infoIndex[k*2+1]) of m_info
    //
    TempVector<uint32_t> m_info;
    uint32_t* m_infoIndex;

    // Liveness state at block head/tail
    //
    TempBitVector m_atHead;
    TempBitVector m_atTail;

    // Once m_atTail is updated,
    // m_atHead should be updated by m_atTail & m_andMask | m_orMask
    //
    TempBitVector m_andMask;
    TempBitVector m_orMask;

    // Note: Successor and predecessor information are populated by outside logic
    //
    BytecodeLivenessBBInfo** m_successors;
    size_t m_numSuccessors;

    // if m_lastCheckedEpoch is greater than all the m_lastChangedEpoch of its successors, this node has work to update.
    //
    size_t m_lastChangedEpoch;
    size_t m_lastCheckedEpoch;

    bool m_hasPrecedessor;

    BytecodeLivenessBBInfo(TempArenaAllocator& alloc,
                           DeegenBytecodeBuilder::BytecodeDecoder& decoder,
                           BasicBlockUpvalueInfo* bbInfo,
                           size_t numLocals)
        : m_info(alloc)
    {
        m_numBytecodesInBB = bbInfo->m_numBytecodesInBB;
        TestAssert(m_numBytecodesInBB > 0);

        m_firstBytecodeIndex = bbInfo->m_bytecodeIndex;

        m_infoIndex = alloc.AllocateArray<uint32_t>(m_numBytecodesInBB * 2);

        m_atHead.Reset(alloc, numLocals);
        m_atTail.Reset(alloc, numLocals);
        m_successors = alloc.AllocateArray<BytecodeLivenessBBInfo*>(bbInfo->m_numSuccessors);
        m_numSuccessors = bbInfo->m_numSuccessors;
        m_hasPrecedessor = false;

        m_lastChangedEpoch = 0;
        m_lastCheckedEpoch = 0;

        m_bytecodeOffset = alloc.AllocateArray<uint32_t>(m_numBytecodesInBB);

        // Compute the offset of each bytecode
        //
        {
            size_t curBytecodeOffset = bbInfo->m_bytecodeOffset;
            size_t index = m_numBytecodesInBB;
            for (size_t iterCounter = 0; iterCounter < m_numBytecodesInBB; iterCounter++)
            {
                TestAssertIff(iterCounter == m_numBytecodesInBB - 1, curBytecodeOffset == bbInfo->m_terminalNodeBcOffset);
                TestAssert(index > 0);
                index--;
                m_bytecodeOffset[index] = SafeIntegerCast<uint32_t>(curBytecodeOffset);
                curBytecodeOffset = decoder.GetNextBytecodePosition(curBytecodeOffset);
            }
            TestAssert(index == 0);
        }

        for (size_t index = 0; index < m_numBytecodesInBB; index++)
        {
            size_t curBytecodeOffset = m_bytecodeOffset[index];

            // Generate the defs of the bytecode
            //
            {
                BytecodeRWCInfo outputs = decoder.GetDataFlowWriteInfo(curBytecodeOffset);
                for (size_t itemOrd = 0; itemOrd < outputs.GetNumItems(); itemOrd++)
                {
                    BytecodeRWCDesc item = outputs.GetDesc(itemOrd);
                    if (item.IsLocal())
                    {
                        m_info.push_back(SafeIntegerCast<uint32_t>(item.GetLocalOrd()));
                    }
                    else if (item.IsRange())
                    {
                        TestAssert(item.GetRangeLength() >= 0);
                        for (size_t i = 0; i < static_cast<size_t>(item.GetRangeLength()); i++)
                        {
                            m_info.push_back(SafeIntegerCast<uint32_t>(item.GetRangeStart() + i));
                        }
                    }
                }

                m_infoIndex[index * 2] = SafeIntegerCast<uint32_t>(m_info.size());
            }

            // Generate the uses of the bytecode
            //
            {
                BytecodeRWCInfo inputs = decoder.GetDataFlowReadInfo(curBytecodeOffset);
                for (size_t itemOrd = 0; itemOrd < inputs.GetNumItems(); itemOrd++)
                {
                    BytecodeRWCDesc item = inputs.GetDesc(itemOrd);
                    if (item.IsLocal())
                    {
                        m_info.push_back(SafeIntegerCast<uint32_t>(item.GetLocalOrd()));
                    }
                    else if (item.IsRange())
                    {
                        TestAssert(item.GetRangeLength() >= 0);
                        for (size_t i = 0; i < static_cast<size_t>(item.GetRangeLength()); i++)
                        {
                            m_info.push_back(SafeIntegerCast<uint32_t>(item.GetRangeStart() + i));
                        }
                    }
                }

                // Special handling: CreateClosure intrinsic uses all the locals it captures, but except the self reference!
                //
                if (decoder.IsBytecodeIntrinsic<BytecodeIntrinsicInfo::CreateClosure>(curBytecodeOffset))
                {
                    BytecodeIntrinsicInfo::CreateClosure info = decoder.GetBytecodeIntrinsicInfo<BytecodeIntrinsicInfo::CreateClosure>(curBytecodeOffset);
                    TestAssert(info.proto.IsConstant());
                    UnlinkedCodeBlock* createClosureUcb = reinterpret_cast<UnlinkedCodeBlock*>(info.proto.AsConstant().m_value);
                    TestAssert(createClosureUcb != nullptr);

                    BytecodeRWCInfo outputs = decoder.GetDataFlowWriteInfo(curBytecodeOffset);
                    TestAssert(outputs.GetNumItems() == 1);
                    BytecodeRWCDesc item = outputs.GetDesc(0 /*itemOrd*/);
                    TestAssert(item.IsLocal());
                    size_t destLocalOrd = item.GetLocalOrd();

                    for (size_t uvOrd = 0; uvOrd < createClosureUcb->m_numUpvalues; uvOrd++)
                    {
                        UpvalueMetadata& uvmt = createClosureUcb->m_upvalueInfo[uvOrd];
                        if (uvmt.m_isParentLocal)
                        {
                            if (uvmt.m_slot == destLocalOrd)
                            {
                                // This is a self-reference, do nothing
                                // Note that no matter whether this is a mutable reference or not, the value stored in the local
                                // is never read from, before it is overwritten by the output. Therefore this is never a use.
                                //
                            }
                            else
                            {
                                m_info.push_back(uvmt.m_slot);
                            }
                        }
                    }
                }

                // Special handling: UpvalueClose intrinsic uses all the captured locals that it closes
                //
                if (decoder.IsBytecodeIntrinsic<BytecodeIntrinsicInfo::UpvalueClose>(curBytecodeOffset))
                {
                    TestAssert(curBytecodeOffset == bbInfo->m_terminalNodeBcOffset);
                    BytecodeIntrinsicInfo::UpvalueClose info = decoder.GetBytecodeIntrinsicInfo<BytecodeIntrinsicInfo::UpvalueClose>(curBytecodeOffset);
                    TestAssert(info.start.IsLocal());
                    size_t uvCloseStart = info.start.AsLocal();
                    TestAssert(uvCloseStart <= numLocals);

                    for (size_t localOrd = uvCloseStart; localOrd < numLocals; localOrd++)
                    {
                        if (bbInfo->m_isLocalCapturedAtHead.IsSet(localOrd) || bbInfo->m_isLocalCapturedInBB.IsSet(localOrd))
                        {
                            // This value is captured before the UpvalueClose and closed by the upvalue.
                            //
                            m_info.push_back(SafeIntegerCast<uint32_t>(localOrd));
                        }
                    }
                }

                m_infoIndex[index * 2 + 1] = SafeIntegerCast<uint32_t>(m_info.size());
            }
        }

#ifdef TESTBUILD
        // Assert every value is in range
        //
        for (uint32_t val : m_info) { TestAssert(val < numLocals); }
#endif

        // Compute the masks for quick update
        //
        m_andMask.Reset(alloc, numLocals);
        m_atTail.SetAllOne();
        ComputeHeadBasedOnTail(m_andMask /*out*/);

        // Important to compute orMask later, since we want m_atTail to be all zero in the end
        //
        m_orMask.Reset(alloc, numLocals);
        m_atTail.Clear();
        ComputeHeadBasedOnTail(m_orMask /*out*/);

#ifdef TESTBUILD
        // The set of bits that are set to 1 should never overlap with the set of bits that are set to 0
        //
        for (size_t i = 0; i < numLocals; i++)
        {
            TestAssertImp(m_orMask.IsSet(i), m_andMask.IsSet(i));
        }
#endif
    }

    // tmpBv must have length numLocals
    // Set 'tmpBv' to be the new head value based on the current m_atTail. Note that m_atHead is not changed.
    //
    void ComputeHeadBasedOnTail(TempBitVector& tmpBv /*out*/)
    {
        TestAssert(tmpBv.m_length == m_atTail.m_length);
        tmpBv.CopyFromEqualLengthBitVector(m_atTail);

        size_t curIndex = 0;
        size_t curInfoEndIndex = 0;
        size_t totalInfoTerms = 2 * m_numBytecodesInBB;
        uint32_t* infoData = m_info.data();
        while (curInfoEndIndex < totalInfoTerms)
        {
            // Process all the defs
            //
            {
                size_t endIndex = m_infoIndex[curInfoEndIndex];
                curInfoEndIndex++;
                TestAssert(endIndex <= m_info.size());
                TestAssert(curIndex <= endIndex);

                while (curIndex < endIndex)
                {
                    uint32_t defSlot = infoData[curIndex];
                    curIndex++;
                    TestAssert(defSlot < tmpBv.m_length);
                    tmpBv.ClearBit(defSlot);
                }
            }

            // Process all the uses
            //
            {
                TestAssert(curInfoEndIndex < totalInfoTerms);
                size_t endIndex = m_infoIndex[curInfoEndIndex];
                curInfoEndIndex++;
                TestAssert(endIndex <= m_info.size());
                TestAssert(curIndex <= endIndex);

                while (curIndex < endIndex)
                {
                    uint32_t useSlot = infoData[curIndex];
                    curIndex++;
                    TestAssert(useSlot < tmpBv.m_length);
                    tmpBv.SetBit(useSlot);
                }
            }
        }
        TestAssert(curInfoEndIndex == totalInfoTerms);
        TestAssert(curIndex == m_info.size());
    }

    void ComputeHeadBasedOnTailFast(TempBitVector& tmpBv /*out*/)
    {
        TestAssert(tmpBv.m_length == m_atTail.m_length);
        TestAssert(tmpBv.m_length == m_andMask.m_length);
        TestAssert(tmpBv.m_length == m_orMask.m_length);
        size_t allocLen = tmpBv.GetAllocLength();
        for (size_t i = 0; i < allocLen; i++)
        {
            tmpBv.m_data[i] = (m_atTail.m_data[i] & m_andMask.m_data[i]) | m_orMask.m_data[i];
        }
    }

    void ComputePerBytecodeLiveness(BytecodeLiveness& r /*out*/)
    {
        TestAssert(r.m_beforeUse.size() == r.m_afterUse.size());
        TestAssert(m_numBytecodesInBB > 0);

        size_t numLocals = m_atHead.m_length;
        size_t lastBytecodeIndex = m_firstBytecodeIndex + m_numBytecodesInBB - 1;
        TestAssert(lastBytecodeIndex < r.m_beforeUse.size());

        for (size_t bytecodeIndex = m_firstBytecodeIndex; bytecodeIndex <= lastBytecodeIndex; bytecodeIndex++)
        {
            TestAssert(r.m_beforeUse[bytecodeIndex].m_length == 0);
            TestAssert(r.m_afterUse[bytecodeIndex].m_length == 0);
            r.m_beforeUse[bytecodeIndex].Reset(numLocals);
            r.m_afterUse[bytecodeIndex].Reset(numLocals);
        }

        size_t curIndex = 0;
        size_t bytecodeIndex = lastBytecodeIndex;
        uint32_t* infoData = m_info.data();
        for (size_t index = 0; index < m_numBytecodesInBB; index++)
        {
            TestAssert(bytecodeIndex < r.m_afterUse.size());

            // "afterUse" is computed by the next bytecode's "beforeUse" + all defs set to false
            //
            DBitVector& afterUse = r.m_afterUse[bytecodeIndex];
            if (index > 0)
            {
                TestAssert(bytecodeIndex + 1 < r.m_beforeUse.size());
                afterUse.CopyFromEqualLengthBitVector(r.m_beforeUse[bytecodeIndex + 1]);
            }
            else
            {
                afterUse.CopyFromEqualLengthBitVector(m_atTail);
            }

            {
                size_t endIndex = m_infoIndex[index * 2];
                TestAssert(endIndex <= m_info.size() && curIndex <= endIndex);
                while (curIndex < endIndex)
                {
                    uint32_t defSlot = infoData[curIndex];
                    curIndex++;
                    TestAssert(defSlot < afterUse.m_length);
                    afterUse.ClearBit(defSlot);
                }
            }

            // "beforeUse" is computed by "afterUse" + all uses set to true
            //
            DBitVector& beforeUse = r.m_beforeUse[bytecodeIndex];
            beforeUse.CopyFromEqualLengthBitVector(afterUse);

            {
                size_t endIndex = m_infoIndex[index * 2 + 1];
                TestAssert(endIndex <= m_info.size() && curIndex <= endIndex);
                while (curIndex < endIndex)
                {
                    uint32_t useSlot = infoData[curIndex];
                    curIndex++;
                    TestAssert(useSlot < beforeUse.m_length);
                    beforeUse.SetBit(useSlot);
                }
            }

            bytecodeIndex--;
        }
        TestAssert(curIndex == m_info.size());
        TestAssert(bytecodeIndex + 1 == m_firstBytecodeIndex);
    }
};

BytecodeLiveness* WARN_UNUSED BytecodeLiveness::ComputeBytecodeLiveness(CodeBlock* codeBlock, const DfgControlFlowAndUpvalueAnalysisResult& cfUvInfo)
{
    TempArenaAllocator alloc;

    // Sort the basic blocks in reverse order of the starting bytecodeIndex
    // This doesn't affect correctness, but may affect how many iterations we need to reach fixpoint.
    // Why do we sort them by bytecodeIndex? Because that's the heuristic JSC uses..
    //
    TempVector<BasicBlockUpvalueInfo*> bbInReverseOrder(alloc);
    for (BasicBlockUpvalueInfo* bb : cfUvInfo.m_basicBlocks)
    {
        bbInReverseOrder.push_back(bb);
    }

    std::sort(bbInReverseOrder.begin(),
              bbInReverseOrder.end(),
              [](BasicBlockUpvalueInfo* lhs, BasicBlockUpvalueInfo* rhs)
              {
                  TestAssertIff(lhs->m_bytecodeIndex == rhs->m_bytecodeIndex, lhs == rhs);
                  return lhs->m_bytecodeIndex > rhs->m_bytecodeIndex;
              });

#ifdef TESTBUILD
    for (size_t i = 0; i + 1 < bbInReverseOrder.size(); i++)
    {
        TestAssert(bbInReverseOrder[i]->m_bytecodeIndex > bbInReverseOrder[i + 1]->m_bytecodeIndex);
    }
#endif

    size_t numBBs = bbInReverseOrder.size();
    size_t numLocals = codeBlock->m_stackFrameNumSlots;

    BytecodeLivenessBBInfo** bbLivenessInfo = alloc.AllocateArray<BytecodeLivenessBBInfo*>(numBBs, nullptr);

    {
        DeegenBytecodeBuilder::BytecodeDecoder decoder(codeBlock);
        TempUnorderedMap<BasicBlockUpvalueInfo*, BytecodeLivenessBBInfo*> bbInfoMap(alloc);
        for (size_t i = 0; i < numBBs; i++)
        {
            bbLivenessInfo[i] = alloc.AllocateObject<BytecodeLivenessBBInfo>(alloc, decoder, bbInReverseOrder[i], numLocals);
            TestAssert(!bbInfoMap.count(bbInReverseOrder[i]));
            bbInfoMap[bbInReverseOrder[i]] = bbLivenessInfo[i];
        }

        // Set up the successor edges
        //
        for (size_t bbOrd = 0; bbOrd < numBBs; bbOrd++)
        {
            BytecodeLivenessBBInfo* bbInfo = bbLivenessInfo[bbOrd];
            BasicBlockUpvalueInfo* bbUvInfo = bbInReverseOrder[bbOrd];
            TestAssert(bbInfo->m_numSuccessors == bbUvInfo->m_numSuccessors);
            for (size_t succOrd = 0; succOrd < bbInfo->m_numSuccessors; succOrd++)
            {
                TestAssert(bbInfoMap.count(bbUvInfo->m_successors[succOrd]));
                BytecodeLivenessBBInfo* succ = bbInfoMap[bbUvInfo->m_successors[succOrd]];
                bbInfo->m_successors[succOrd] = succ;
                succ->m_hasPrecedessor = true;
            }
        }
    }

    // Propagate to fixpoint
    //
    {
        TempBitVector tmpBv;
        tmpBv.Reset(alloc, numLocals);

        size_t currentEpoch = 1;
        bool isFirstIteration = true;
        while (true)
        {
            bool needMoreIterations = false;
            for (size_t bbOrd = 0; bbOrd < numBBs; bbOrd++)
            {
                BytecodeLivenessBBInfo* bb = bbLivenessInfo[bbOrd];

                // Our tail value could potentially change if one of our successor's head value has received an update
                // after the last time we checked them (or if this is the first iteration).
                //
                bool shouldCheck = false;
                for (size_t i = 0; i < bb->m_numSuccessors; i++)
                {
                    if (bb->m_successors[i]->m_lastChangedEpoch > bb->m_lastCheckedEpoch)
                    {
                        shouldCheck = true;
                        break;
                    }
                }

                if (shouldCheck || isFirstIteration)
                {
                    currentEpoch++;
                    bb->m_lastCheckedEpoch = currentEpoch;

                    // Initialize tmpBv to be the union of all the successors' head state
                    //
                    {
                        tmpBv.Clear();
                        uint64_t* tmpBvData = tmpBv.m_data.get();
                        size_t tmpBvAllocLength = tmpBv.GetAllocLength();
                        for (size_t succOrd = 0; succOrd < bb->m_numSuccessors; succOrd++)
                        {
                            BytecodeLivenessBBInfo* succ = bb->m_successors[succOrd];
                            TestAssert(succ->m_atHead.m_length == tmpBv.m_length);
                            uint64_t* srcData = succ->m_atHead.m_data.get();
                            for (size_t i = 0; i < tmpBvAllocLength; i++)
                            {
                                tmpBvData[i] |= srcData[i];
                            }
                        }
                    }

                    // Set bb->m_atTail to be tmpBv and check if it changes ithe tail value
                    //
                    bool tailChanged = UpdateBitVectorAfterMonotonicPropagation(bb->m_atTail /*inout*/, tmpBv /*copyFrom*/);

                    if (tailChanged || isFirstIteration)
                    {
                        // Compute the new head state from the tail state, and store it into tmpBv
                        //
                        bb->ComputeHeadBasedOnTailFast(tmpBv /*out*/);

                        // Set bb->m_head to tmpBv and check if it changes the head value
                        //
                        bool headChanged = UpdateBitVectorAfterMonotonicPropagation(bb->m_atHead /*inout*/, tmpBv /*copyFrom*/);

#ifdef TESTBUILD
                        // In test build, assert that ComputeHeadBasedOnTailFast produces the same result as ComputeHeadBasedOnTail
                        //
                        bb->ComputeHeadBasedOnTail(tmpBv /*out*/);
                        for (size_t i = 0; i < tmpBv.GetAllocLength(); i++)
                        {
                            TestAssert(tmpBv.m_data[i] == bb->m_atHead.m_data[i]);
                        }
#endif
                        // We do not need to update lastChangedEpoch if only tail changed but head did not change,
                        // since all our predecessors only look at our head, never our tail.
                        // Similarly, needMoreIterations is also not updated, since there's nothing in our state changed so that it can affect others.
                        //
                        if (headChanged)
                        {
                            currentEpoch++;
                            bb->m_lastChangedEpoch = currentEpoch;
                            // If we do not have predecessor, our state change cannot affect anyone else.
                            //
                            if (bb->m_hasPrecedessor)
                            {
                                needMoreIterations = true;
                            }
                        }
                    }
                }
            }

            if (!needMoreIterations)
            {
                break;
            }
            isFirstIteration = false;
        }
    }

#ifdef TESTBUILD
    // Assert that fixpoint is indeed reached
    //
    {
        TempBitVector tmpBv;
        tmpBv.Reset(alloc, numLocals);

        for (size_t bbOrd = 0; bbOrd < numBBs; bbOrd++)
        {
            BytecodeLivenessBBInfo* bb = bbLivenessInfo[bbOrd];

            tmpBv.Clear();
            for (size_t succOrd = 0; succOrd < bb->m_numSuccessors; succOrd++)
            {
                BytecodeLivenessBBInfo* succ = bb->m_successors[succOrd];
                TestAssert(succ->m_atHead.m_length == tmpBv.m_length);
                for (size_t i = 0; i < tmpBv.m_length; i++)
                {
                    if (succ->m_atHead.IsSet(i))
                    {
                        tmpBv.SetBit(i);
                    }
                }
            }

            TestAssert(bb->m_atTail.m_length == tmpBv.m_length);
            for (size_t i = 0; i < tmpBv.m_length; i++)
            {
                TestAssertIff(tmpBv.IsSet(i), bb->m_atTail.IsSet(i));
            }

            bb->ComputeHeadBasedOnTail(tmpBv /*out*/);

            TestAssert(bb->m_atHead.m_length == tmpBv.m_length);
            for (size_t i = 0; i < tmpBv.m_length; i++)
            {
                TestAssertIff(tmpBv.IsSet(i), bb->m_atHead.IsSet(i));
            }
        }
    }
#endif

    // Compute the liveness state for each bytecode
    //
    TestAssert(codeBlock->m_baselineCodeBlock != nullptr);
    size_t numBytecodes = codeBlock->m_baselineCodeBlock->m_numBytecodes;

    BytecodeLiveness* r = DfgAlloc()->AllocateObject<BytecodeLiveness>();
    TestAssert(r->m_beforeUse.size() == 0);
    TestAssert(r->m_afterUse.size() == 0);
    r->m_beforeUse.resize(numBytecodes);
    r->m_afterUse.resize(numBytecodes);

    for (size_t bbOrd = 0; bbOrd < numBBs; bbOrd++)
    {
        BytecodeLivenessBBInfo* bb = bbLivenessInfo[bbOrd];
        bb->ComputePerBytecodeLiveness(*r /*out*/);
    }

    // It's possible that the bytecode stream contains trivially unreachable bytecodes
    // (e.g., the source function contains a dead loop followed by a bunch of code),
    // in which case those bytecodes will not show up in any basic blocks.
    // Users of this class should never need to query liveness info for those bytecodes,
    // but for sanity, allocate arrays for those bytecodes (with everything is dead) as well.
    //
    for (size_t bytecodeIndex = 0; bytecodeIndex < numBytecodes; bytecodeIndex++)
    {
        if (r->m_beforeUse[bytecodeIndex].m_length == 0)
        {
            TestAssert(r->m_afterUse[bytecodeIndex].m_length == 0);
            r->m_beforeUse[bytecodeIndex].Reset(numLocals);
            r->m_afterUse[bytecodeIndex].Reset(numLocals);
        }

        TestAssert(r->m_beforeUse[bytecodeIndex].m_length == numLocals);
        TestAssert(r->m_afterUse[bytecodeIndex].m_length == numLocals);
    }

    // Unfortunately there isn't much more that we can assert.
    // We allow bytecodes to use undefined values, and our parser in fact will generate such bytecodes
    // in rare cases (specifically, the ISTC and ISFC bytecodes). Which is unfortunate, but that's what
    // we have in hand..
    //
    // So it's possible that a local that is not an argument is live at function entry, or a bytecode
    // used a value that is live in our analysis but actually clobbered by a previous bytecode, etc..
    //
    // But as long as our liveness result is an overapproximation of the real liveness (i.e., we never
    // report something is dead when it is actually live), we are good.
    //
    return r;
}

}   // namespace dfg
