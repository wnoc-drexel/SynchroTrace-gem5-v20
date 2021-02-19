/*
 * Copyright (c) 2015-2016 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * Copyright (c) 2019, Drexel University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Karthik Sangaiah
 *          Ankit More
 *          Mike Lui
 *
 * Parses Prism event trace files and Prism Pthread file
 */

#include "scn/scn.h"
#include "st_parser.hh"


constexpr StEvent::ComputeTagType StEvent::ComputeTag;
constexpr StEvent::MemoryTagType StEvent::MemoryTag;
constexpr StEvent::MemoryCommTagType StEvent::MemoryCommTag;
constexpr StEvent::ThreadApiTagType StEvent::ThreadApiTag;
constexpr StEvent::InsnMarkerTagType StEvent::InsnMarkerTag;
constexpr StEvent::TraceEventMarkerTagType StEvent::TraceEventMarkerTag;
constexpr StEvent::EndTagType StEvent::EndTag;


//----------------------------------------------------------------------------
StTraceParser::StTraceParser(
  uint32_t bytesPerCacheBlock, uint64_t bytesInMainMemoryTotal)
  : m_bytesPerCacheBlock{bytesPerCacheBlock},
    m_bytesInMainMemoryTotal{bytesInMainMemoryTotal},
    m_maskForCacheOffset{bytesPerCacheBlock-1},
    rng(std::chrono::high_resolution_clock::now().time_since_epoch().count()),
    chunkedWriteAddrs{},
    chunkedReadAddrs{}
{
    fatal_if(maxBytesPerRequest > bytesPerCacheBlock,
             "cache block is too small for SynchroTrace replayer",
             bytesPerCacheBlock);
    fatal_if(!isPowerOf2(m_bytesPerCacheBlock),
             "cache Block size is not a power of 2");
    fatal_if(m_bytesInMainMemoryTotal % m_bytesPerCacheBlock != 0,
             "unsupported physical memory size: "
             "not a multiple of cache block size");
}

template<typename F>
void StTraceParser::foreach_MemAddrChunk(
  uintptr_t startAddr, uintptr_t endAddr, F f)
{
    // Take a range of memory, and split it into accesses.
    // Because SynchroTrace workload capture may compress mutliple consecutive
    // address ranges into a single range, the range may be artificially large.
    // (i.e. not representing a single actual access)
    const uint32_t totalBytesRemaining = endAddr - startAddr + 1;
    assert(endAddr - startAddr + 1 <
           std::numeric_limits<decltype(totalBytesRemaining)>::max());

    // Constrain virtual address to fit within physical memory.
    // The offset into the cache block is to be the same between
    // virtual and physical addresses.
    // This is guaranteed by checks during construction of a parser.
    uintptr_t currReqAddr = {startAddr % m_bytesInMainMemoryTotal};
    const uintptr_t finalAddr = {currReqAddr + totalBytesRemaining - 1};

    // At this point, we still might spill out of physical memory depending on
    // the size of the request, so we check if we should further split the
    // address.
    uint32_t spillBytes = finalAddr > m_bytesInMainMemoryTotal ?
        finalAddr - m_bytesInMainMemoryTotal : 0;
    fatal_if(spillBytes > m_bytesInMainMemoryTotal,
             "SynchroTrace replay detected a memory request "
             "greater than physical memory limits");

    uint32_t currBytesRemaining = {totalBytesRemaining - spillBytes};
    uint32_t prevReqSize = 0;
    while (currBytesRemaining > 0)
    {
        currReqAddr = {currReqAddr + prevReqSize};
        const uint32_t currReqSize =
            {std::min(currBytesRemaining, uint32_t{maxBytesPerRequest})};

        // Adjust the request to not cross cache lines.
        // XXX MDL20190609 Doesn't Ruby handle this?
        const uintptr_t offset = currReqAddr & m_maskForCacheOffset;
        const uint32_t bytesAvailableForReqOffset =
            {m_bytesPerCacheBlock - static_cast<uint32_t>(offset)};
        const uint32_t adjustedReqSize =
            {std::min(currReqSize, bytesAvailableForReqOffset)};

        f(currReqAddr, adjustedReqSize);
        prevReqSize = adjustedReqSize;
        currBytesRemaining -= adjustedReqSize;
    }

    // chunk up any remaining bytes if this request spilled over
    if (spillBytes)
    {
        assert(currReqAddr + prevReqSize == 0);
        foreach_MemAddrChunk(0, spillBytes-1, f);
    }
}

void
StTraceParser::parseTo(std::vector<StEvent>& buffer,
                       std::string& line,
                       ThreadID threadId,
                       StEventID& eventId)
{
    // Place a trace event marker before every "real" trace event.
    // Then decompose the trace event into replay events and place
    // them in the event stream.

    switch (line[0])
    {
    case COMP_EVENT_TOKEN:
        buffer.emplace_back(StEvent::TraceEventMarkerTag, eventId);
        parseCompEventTo(buffer, line, threadId, eventId);
        ++eventId;
        break;
    case COMM_EVENT_TOKEN:
        buffer.emplace_back(StEvent::TraceEventMarkerTag, eventId);
        parseCommEventTo(buffer, line, threadId, eventId);
        ++eventId;
        break;
    case THREADAPI_EVENT_TOKEN:
        buffer.emplace_back(StEvent::TraceEventMarkerTag, eventId);
        parseThreadEventTo(buffer, line, threadId, eventId);
        ++eventId;
        break;
    case MARKER_EVENT_TOKEN:
        parseMarkerEventTo(buffer, line, threadId, eventId);
        break;
    default:
        fatal("Invalid line detected in thread: %d\n", threadId);
    }
}

void
StTraceParser::parseCompEventTo(std::vector<StEvent>& buffer,
                                std::string& line,
                                ThreadID threadId,
                                StEventID eventId)
{
    // Compute events are iops/flops and intra-thread reads/writes that have
    // been combined into a single "event".
    //
    // Memory addresses may be compressed toghether. This is for saving storage
    // on very large traces.
    // This information is lost when the trace is created, so we need to do
    // extra work to infer/decompress the original trace for replay.
    //
    // For example, we may have:
    // - 1 read of 100-bytes, stored as a single 100-byte range.
    // OR
    // - 100 reads of consecutive 1-bytes, stored as a single 100-byte range.
    // OR
    // - anywhere in between.
    //
    // To reconstruct the trace, we use a heuristic of splitting up large
    // address ranges, if too large, into predetermined chunk sizes, and then
    // distributing reads/writes amongst the resulting address ranges.
    // For example, if we have:
    // - 16 reads over 2, 32-byte range:
    //   - the 2, 32-byte ranges may be split into 8, 8-byte ranges
    //   - the 16 reads are distributed over the 8 ranges
    //   - resulting in 16 reads, 2 reads on each range
    // OR
    // - 1 read over 1, 64-byte range:
    //   - the 64-byte range may be split into 8, 8-byte ranges
    //   - the 1 reads is split into 8 reads over each range
    //   - resulting in 8 reads, 1 read on each range
    //
    // Thus, we do not know how many read/write events we'll have, given
    // the SynchroTrace event, until after the entire line has been parsed.
    //
    // Finally, pure compute events (iops/flops) are intermixed with the memory
    // accesses to provide an average (compute/memory access) timing for
    // replay.
    //
    // Example line:
    //
    // @ 661,0,100,44 $ 0x402b110 0x402b113 $ 0xeffe968 0xeffe96f * 0x44f 0x450

    uint64_t iops, flops, reads, writes, start, end;
    chunkedWriteAddrs.clear();
    chunkedReadAddrs.clear();
    auto s = scn::make_stream(line);

    auto res = scn::scan(s,
                         "@ {:d},{:d},{:d},{:d} ",
                         iops,
                         flops,
                         reads,
                         writes);
    fatal_if(!res, "error parsing compute trace event");

    // generate chunked up write/read accesses
    while (scn::scan(s, "$ {:x} {:x} ", start, end))
        foreach_MemAddrChunk(start,
                             end,
                             [&] (uintptr_t addr, uint32_t bytes) {
                                chunkedWriteAddrs.push_back(
                                    {addr, bytes, ReqType::REQ_WRITE});
                             });
    while (scn::scan(s, "* {:x} {:x} ", start, end))
        foreach_MemAddrChunk(start,
                             end,
                             [&] (uintptr_t addr, uint32_t bytes) {
                                chunkedReadAddrs.push_back(
                                    {addr, bytes, ReqType::REQ_READ});
                             });

    const uint64_t chunkedReadsSize = {chunkedReadAddrs.size()};
    const uint64_t chunkedWritesSize = {chunkedWriteAddrs.size()};
    const uint64_t totalLocalReads = {std::max(reads, chunkedReadsSize)};
    const uint64_t totalLocalWrites = {std::max(writes, chunkedWritesSize)};
    const uint64_t totalMemoryAccesses = {totalLocalReads + totalLocalWrites};

    if (totalMemoryAccesses == 0)
    {
        // Create a single sub event if there are no memory ops.
        buffer.emplace_back(StEvent::ComputeTag, iops, flops);
    }
    else
    {
        // Mark off memory accesses and distribute the IOPS and FLOPS evenly.
        // HEURISTIC: Split up compute ops evenly across the number of accesses
        const uint64_t iopsPerAccess  = {iops  / totalMemoryAccesses};
        const uint64_t iopsLeftover  = {iops  % totalMemoryAccesses};
        const uint64_t flopsPerAccess = {flops / totalMemoryAccesses};
        const uint64_t flopsLeftover = {flops % totalMemoryAccesses};

        uint64_t readsInserted = 0;
        uint64_t writesInserted = 0;

        // Alternate between writes and reads
        ReqType type = totalLocalWrites > 0 ?
          ReqType::REQ_WRITE : ReqType::REQ_READ;

        for (uint64_t i = 0; i < totalMemoryAccesses; i++)
        {
            // Create a compute sub-event to "wait" before issuing the
            // following memory request.
            buffer.emplace_back(StEvent::ComputeTag,
                                iopsPerAccess,
                                flopsPerAccess);

            switch(type)
            {
                case ReqType::REQ_READ:
                    buffer.emplace_back(StEvent::MemoryTag,
                                        chunkedReadAddrs[readsInserted %
                                                         chunkedReadsSize]);
                    readsInserted++;
                    break;
                case ReqType::REQ_WRITE:
                    buffer.emplace_back(StEvent::MemoryTag,
                                        chunkedWriteAddrs[writesInserted %
                                                          chunkedWritesSize]);
                    writesInserted++;
                    break;
                default:
                    panic("Invalid memory request type.\n");
            }

            // Flip the type if possible
            type = (type == ReqType::REQ_WRITE &&
                    readsInserted < totalLocalReads) ?
                ReqType::REQ_READ : (writesInserted < totalLocalWrites) ?
                ReqType::REQ_WRITE :
                // sanity check: should either have reads left,
                // or this is the last memory access
                (assert(readsInserted < totalLocalReads ||
                        (writesInserted + readsInserted ==
                         totalMemoryAccesses)),
                 ReqType::REQ_READ);
        }

        // Distribute the residuals randomly amongst the generated compute
        // events. Note that this randomness means simulations will not be
        // deterministic.
        std::uniform_int_distribution<size_t> gen(0, totalMemoryAccesses-1);

        // offset to the last generated compute event
        const size_t len = buffer.size() - 2;

        for (uint64_t j = 0; j < iopsLeftover; j++)
        {
            const size_t idx = len - gen(rng)*2;
            StEvent& ev = buffer[idx];
            assert(ev.tag == StEvent::Tag::COMPUTE);
            ev.computeOps.iops++;
        }
        for (uint64_t j = 0; j < flopsLeftover; j++)
        {
            const size_t idx = len - gen(rng)*2;
            StEvent& ev = buffer[idx];
            assert(ev.tag == StEvent::Tag::COMPUTE);
            ev.computeOps.flops++;
        }
    }
}

void
StTraceParser::parseCommEventTo(std::vector<StEvent>& buffer,
                                std::string& line,
                                ThreadID threadId,
                                StEventID eventId)
{
    // For communication events, create read-based sub events for each
    // dependency.
    // Example line:
    //
    // # 5 8388778 0x403e290 0x403e297

    uint64_t prodThreadId, prodEventId, start, end;
    auto s = scn::make_stream(line);
    while (scn::scan(s, "# {:d} {:d} {:x} {:x} ",
                     prodThreadId,
                     prodEventId,
                     start,
                     end))
    {
        fatal_if(prodThreadId > std::numeric_limits<ThreadID>::max() ||
                 prodEventId > std::numeric_limits<StEventID>::max(),
                 "invalid data detected while parsing comm event: "
                 "Producer Thread<%d>:Producer Event<%d>",
                 prodThreadId,
                 prodEventId);

        foreach_MemAddrChunk(start,
                             end,
                             [=, &buffer](uintptr_t addr, uint32_t bytes) {
                                buffer.emplace_back(StEvent::MemoryCommTag,
                                                    addr,
                                                    bytes,
                                                    prodEventId,
                                                    prodThreadId);
                                });
    }
}

void
StTraceParser::parseThreadEventTo(std::vector<StEvent>& buffer,
                                  std::string& line,
                                  ThreadID threadId,
                                  StEventID eventId)
{
    // example line:
    //
    // ^ 4^0xad97700
    //
    // or for conditional wait/signal special case:
    //
    // ^ 6^0xad97700&0xdeadbeef

    using IntegralType = std::underlying_type<ThreadApi::EventType>::type;
    constexpr auto max = static_cast<IntegralType>(
        ThreadApi::EventType::NUM_TYPES);

    uint64_t typeVal, pthAddr;
    auto s = scn::make_stream(line);
    auto res = scn::scan(s, "^ {:d}^{:x}", typeVal, pthAddr);
    fatal_if(!res, "error parsing pthread api trace event");

    // pthread event type
    assert(typeVal < max);
    const ThreadApi::EventType type =
        static_cast<ThreadApi::EventType>(typeVal);

    // the lock variable for the conditional signal/wait
    Addr mutexLockAddr = 0;
    scn::scan(s, "&{:x}", mutexLockAddr);

    buffer.emplace_back(StEvent::ThreadApiTag, type, pthAddr, mutexLockAddr);
}

void
StTraceParser::parseMarkerEventTo(std::vector<StEvent>& buffer,
                                  std::string& line,
                                  ThreadID threadId,
                                  StEventID eventId)
{
    // example line:
    //
    // ! 4096

    uint64_t insns;
    auto s = scn::make_stream(line);
    auto res = scn::scan(s, "! {:d}", insns);
    fatal_if(!res, "error parsing instruction marker event");
    buffer.emplace_back(StEvent::InsnMarkerTag, insns);
}


//----------------------------------------------------------------------------
StEventStream::StEventStream(ThreadID threadId,
                             const std::string& eventDir,
                             uint32_t bytesPerCacheBlock,
                             uint64_t bytesInMainMemoryTotal,
                             size_t initialBufferSize)
  : threadId{threadId},
    lastEventIdParsed{0},
    lastLineParsed{0},
    filename{csprintf("%s/sigil.events.out-%d.gz",
                      eventDir.c_str(),
                      threadId)},
    line{},
    traceFile{filename.c_str()},
    parser{bytesPerCacheBlock, bytesInMainMemoryTotal},
    buffer{}
{
    fatal_if(!traceFile, "failed to open file: %s\n", filename);

    buffer.reserve(initialBufferSize);
    buffer_current = buffer.cbegin();
    buffer_end = buffer.cend();

    eventsPerFill = {initialBufferSize/8};
    // Heuristic to "partial-fill" the buffer at a time.
    // Note that one event from the trace file may decompress to MANY
    // events in the buffer. Thus, we use a vector which can expand to fit
    // extra events. Even if the buffer is expanded beyond its original
    // size, we still use the same amount on each fill.

    // initial buffer fill
    refill();
}

const StEvent&
StEventStream::peek()
{
    return *buffer_current;
}

void
StEventStream::pop()
{
    buffer_current++;

    if (buffer_current == buffer_end)
        refill();
}

void
StEventStream::refill()
{
    buffer.clear();
    for (size_t i = 0; i < eventsPerFill; i++)
    {
        if (std::getline(traceFile, line))
        {
            ++lastLineParsed;

            // the event id is incremented by the parser
            parser.parseTo(buffer, line, threadId, lastEventIdParsed);
        }
        else
        {
            buffer.emplace_back(StEvent::EndTag);
            break;
        }
    }
    buffer_current = buffer.cbegin();
    buffer_end = buffer.cend();
}

//----------------------------------------------------------------------------
StTracePthreadMetadata::StTracePthreadMetadata(const std::string& eventDir)
{
    std::string filename = {csprintf("%s/sigil.pthread.out",
                                     eventDir.c_str())};
    std::ifstream pthFile{filename};
    fatal_if(!pthFile, "failed to open file: %s\n", filename);

    parsePthreadFile(pthFile);
}

void
StTracePthreadMetadata::parsePthreadFile(std::ifstream& pthFile)
{
    assert(pthFile.good());

    std::string line;
    while (std::getline(pthFile, line))
    {
        assert(line.length() > 2);
        if (line[0] == ADDR_TO_TID_TOKEN &&
            line[1] == ADDR_TO_TID_TOKEN)
            parseAddressToID(line);
        else if (line[0] == BARRIER_TOKEN &&
                 line[1] == BARRIER_TOKEN)
            parseBarrierEvent(line);
    }
}

void
StTracePthreadMetadata::parseAddressToID(std::string& line)
{
    // example:
    // ##80881984,2

    uint64_t pthAddr, threadId;
    auto s = scn::make_stream(line);
    auto res = scn::scan(s, "##{:d},{:d}", pthAddr, threadId);
    fatal_if(!res, "error parsing thread create from pthread file");

    // Keep Thread IDs in a map.
    // The trace has base-1 thread ids, but replay expects base-0.
    auto p = m_addressToIdMap.insert({pthAddr, threadId - 1});
    assert(p.second);
}

void
StTracePthreadMetadata::parseBarrierEvent(std::string& line)
{
    // example:
    // **67117104,1,2,3,4,5,6,7,8

    uint64_t pthAddr, threadId;
    auto s = scn::make_stream(line);
    auto res = scn::scan(s, "**{:d}", pthAddr);
    fatal_if(!res, "error parsing barrier from file");
    while (scn::scan(s, ",{:d}", threadId))
        m_barrierMap[pthAddr].insert(threadId - 1);
}
