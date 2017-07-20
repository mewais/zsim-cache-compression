/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <limits>

#include "cache_arrays.h"
#include "hash.h"
#include "repl_policies.h"
#include "zsim.h"

/* Set-associative array implementation */

SetAssocArray::SetAssocArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf) : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc)  {
    array = gm_calloc<Address>(numLines);
    numSets = numLines/assoc;
    setMask = numSets - 1;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

int32_t SetAssocArray::lookup(const Address lineAddr, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (array[id] ==  lineAddr) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

uint32_t SetAssocArray::preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr) { //TODO: Give out valid bit of wb cand?
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;

    uint32_t candidate = rp->rankCands(req, SetAssocCands(first, first+assoc));

    *wbLineAddr = array[candidate];
    return candidate;
}

void SetAssocArray::postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate) {
    rp->replaced(candidate);
    array[candidate] = lineAddr;
    rp->update(candidate, req);
}

// Doppelganger Start
DoppelgangerTagArray::DoppelgangerTagArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf) : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc)  {
    tagArray = gm_calloc<Address>(numLines);
    prevPointerArray = gm_calloc<int32_t>(numLines);
    nextPointerArray = gm_calloc<int32_t>(numLines);
    mapPointerArray = gm_calloc<int32_t>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        prevPointerArray[i] = -1;
        nextPointerArray[i] = -1;
        mapPointerArray[i] = -1;
    }
    numSets = numLines/assoc;
    setMask = numSets - 1;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

int32_t DoppelgangerTagArray::lookup(const Address lineAddr, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (tagArray[id] ==  lineAddr) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

int32_t DoppelgangerTagArray::preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;

    uint32_t candidate = rp->rankCands(req, SetAssocCands(first, first+assoc));

    *wbLineAddr = tagArray[candidate];
    return candidate;
}

bool DoppelgangerTagArray::evictAssociatedData(const int32_t lineId, int32_t* newLLHead) {
    *newLLHead = -1;
    if (mapPointerArray[lineId] == -1)
        return false;
    if (prevPointerArray[lineId] != -1)
        return false;
    else
        *newLLHead = nextPointerArray[lineId];
    if (nextPointerArray[lineId] != -1)
        return false;
    return true;
}

void DoppelgangerTagArray::postinsert(const Address lineAddr, const MemReq* req, const int32_t tagId, const int32_t mapId, const int32_t listHead, const bool updateReplacement) {
    rp->replaced(tagId);
    tagArray[tagId] = lineAddr;
    mapPointerArray[tagId] = mapId;
    if (prevPointerArray[tagId] != -1)
        nextPointerArray[prevPointerArray[tagId]] = nextPointerArray[tagId];
    if (nextPointerArray[tagId] != -1)
        prevPointerArray[nextPointerArray[tagId]] = prevPointerArray[tagId];
    prevPointerArray[tagId] = -1;
    nextPointerArray[tagId] = listHead;
    if (listHead >= 0) {
        if(prevPointerArray[listHead] == -1) prevPointerArray[listHead] = tagId;
        else panic("List head is not actually a list head!");
    }
    if(updateReplacement) rp->update(mapId, req);
}

int32_t DoppelgangerTagArray::readMapId(const int32_t tagId) {
    return mapPointerArray[tagId];
}

Address DoppelgangerTagArray::readAddress(const int32_t tagId) {
    return tagArray[tagId];
}

int32_t DoppelgangerTagArray::readNextLL(const int32_t tagId) {
    return nextPointerArray[tagId];
}

void DoppelgangerTagArray::print() {
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (mapPointerArray[i] != -1)
            info("%i: %lu, %i, %i, %i", i, tagArray[i] << lineBits, prevPointerArray[i], nextPointerArray[i], mapPointerArray[i]);
    }
}

DoppelgangerDataArray::DoppelgangerDataArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf) : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc)  {
    mtagArray = gm_calloc<int32_t>(numLines);
    tagPointerArray = gm_calloc<int32_t>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        tagPointerArray[i] = -1;
        mtagArray[i] = -1;
    }
    numSets = numLines/assoc;
    setMask = numSets - 1;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

int32_t DoppelgangerDataArray::lookup(const uint32_t map, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, map) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (mtagArray[id] ==  (int32_t)map) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

uint32_t DoppelgangerDataArray::calculateMap(const DataLine data, const DataType type, const DataValue minValue, const DataValue maxValue) {
    // Get hash and map values
    int64_t intAvgHash = 0, intRangeHash = 0;
    double floatAvgHash = 0, floatRangeHash = 0;
    int64_t intMax = std::numeric_limits<int64_t>::min(), 
            intMin = std::numeric_limits<int64_t>::max(),
            intSum = 0;
    double floatMax = std::numeric_limits<double>::min(),
            floatMin = std::numeric_limits<double>::max(),
            floatSum = 0;
    double mapStep = 0;
    int32_t avgMap = 0, rangeMap = 0;
    uint32_t map = 0;
    switch (type)
    {
        case ZSIM_UINT8:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint8_t); i++) {
                intSum += ((uint8_t*) data)[i];
                if (((uint8_t*) data)[i] > intMax)
                    intMax = ((uint8_t*) data)[i];
                if (((uint8_t*) data)[i] < intMin)
                    intMin = ((uint8_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint8_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.UINT8)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.UINT8)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(uint8_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.UINT8 - minValue.UINT8)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_INT8:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int8_t); i++) {
                intSum += ((int8_t*) data)[i];
                if (((int8_t*) data)[i] > intMax)
                    intMax = ((int8_t*) data)[i];
                if (((int8_t*) data)[i] < intMin)
                    intMin = ((int8_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int8_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT8)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT8)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(int8_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.INT8 - minValue.INT8)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_UINT16:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint16_t); i++) {
                intSum += ((uint16_t*) data)[i];
                if (((uint16_t*) data)[i] > intMax)
                    intMax = ((uint16_t*) data)[i];
                if (((uint16_t*) data)[i] < intMin)
                    intMin = ((uint16_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint16_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.UINT16)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.UINT16)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(uint16_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.UINT16 - minValue.UINT16)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_INT16:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int16_t); i++) {
                intSum += ((int16_t*) data)[i];
                if (((int16_t*) data)[i] > intMax)
                    intMax = ((int16_t*) data)[i];
                if (((int16_t*) data)[i] < intMin)
                    intMin = ((int16_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int16_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT16)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT16)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(int16_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.INT16 - minValue.INT16)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_UINT32:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint32_t); i++) {
                intSum += ((uint32_t*) data)[i];
                if (((uint32_t*) data)[i] > intMax)
                    intMax = ((uint32_t*) data)[i];
                if (((uint32_t*) data)[i] < intMin)
                    intMin = ((uint32_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint32_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.UINT32)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.UINT32)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.UINT32 - minValue.UINT32)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_INT32:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int32_t); i++) {
                intSum += ((int32_t*) data)[i];
                if (((int32_t*) data)[i] > intMax)
                    intMax = ((int32_t*) data)[i];
                if (((int32_t*) data)[i] < intMin)
                    intMin = ((int32_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int32_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT32)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT32)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.INT32 - minValue.INT32)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_UINT64:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint64_t); i++) {
                intSum += ((uint64_t*) data)[i];
                if ((int64_t)(((uint64_t*) data)[i]) > intMax)
                    intMax = ((uint64_t*) data)[i];
                if ((int64_t)(((uint64_t*) data)[i]) < intMin)
                    intMin = ((uint64_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint64_t));
            intRangeHash = intMax - intMin;
            if (intMax > (int64_t)maxValue.UINT64)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < (int64_t)minValue.UINT64)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.UINT64 - minValue.UINT64)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_INT64:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int64_t); i++) {
                intSum += ((int64_t*) data)[i];
                if (((int64_t*) data)[i] > intMax)
                    intMax = ((int64_t*) data)[i];
                if (((int64_t*) data)[i] < intMin)
                    intMin = ((int64_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int64_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT64)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT64)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.INT64 - minValue.INT64)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_FLOAT:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(float); i++) {
                floatSum += ((float*) data)[i];
                if (((float*) data)[i] > floatMax)
                    floatMax = ((float*) data)[i];
                if (((float*) data)[i] < floatMin)
                    floatMin = ((float*) data)[i];
            }
            floatAvgHash = floatSum/(zinfo->lineSize/sizeof(float));
            floatRangeHash = floatMax - floatMin;
            if (floatMax > maxValue.FLOAT)
                panic("Received a value bigger than the annotation's Max!! %f, %f", floatMax, maxValue.FLOAT);
            if (floatMin < minValue.FLOAT)
                panic("Received a value lower than the annotation's Min!! %f, %f", floatMax, maxValue.FLOAT);
            mapStep = (maxValue.FLOAT - minValue.FLOAT)/std::pow(2,zinfo->mapSize-1);
            avgMap = floatAvgHash/mapStep;
            rangeMap = floatRangeHash/mapStep;
            break;
        case ZSIM_DOUBLE:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(double); i++) {
                floatSum += ((double*) data)[i];
                if (((double*) data)[i] > floatMax)
                    floatMax = ((double*) data)[i];
                if (((double*) data)[i] < floatMin)
                    floatMin = ((double*) data)[i];
            }
            floatAvgHash = floatSum/(zinfo->lineSize/sizeof(double));
            floatRangeHash = floatMax - floatMin;
            if (floatMax > maxValue.DOUBLE)
                panic("Received a value bigger than the annotation's Max!!");
            if (floatMin < minValue.DOUBLE)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.DOUBLE - minValue.DOUBLE)/std::pow(2,zinfo->mapSize-1);
            avgMap = floatAvgHash/mapStep;
            rangeMap = floatRangeHash/mapStep;
            break;
        default:
            panic("Wrong Data Type!!");
    }
    map = ((uint32_t)avgMap << (32 - zinfo->mapSize)) >> (32 - zinfo->mapSize);
    rangeMap = ((uint32_t)rangeMap << (32 - zinfo->mapSize)) >> (32 - zinfo->mapSize);
    map |= (rangeMap >> (zinfo->mapSize/2)) << zinfo->mapSize;
    return map;
}

int32_t DoppelgangerDataArray::preinsert(const uint32_t map, const MemReq* req, int32_t* tagId) {
    uint32_t set = hf->hash(0, map) & setMask;
    uint32_t first = set*assoc;

    uint32_t mapId = rp->rankCands(req, SetAssocCands(first, first+assoc));

    *tagId = tagPointerArray[mapId];
    return mapId;
}

void DoppelgangerDataArray::postinsert(const uint32_t map, const MemReq* req, int32_t mapId, int32_t tagId, bool updateReplacement) {
    rp->replaced(mapId);
    mtagArray[mapId] = map;
    tagPointerArray[mapId] = tagId;
    if(updateReplacement) rp->update(mapId, req);
}

int32_t DoppelgangerDataArray::readListHead(const int32_t mapId) {
    return tagPointerArray[mapId];
}

int32_t DoppelgangerDataArray::readMap(const int32_t mapId) {
    return mtagArray[mapId];
}

void DoppelgangerDataArray::print() {
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (tagPointerArray[i] != -1)
            info("%i: %u, %i", i, mtagArray[i], tagPointerArray[i]);
    }
}
// Doppelganger End

/* ZCache implementation */

ZArray::ZArray(uint32_t _numLines, uint32_t _ways, uint32_t _candidates, ReplPolicy* _rp, HashFamily* _hf) //(int _size, int _lineSize, int _assoc, int _zassoc, ReplacementPolicy<T>* _rp, int _hashType)
    : rp(_rp), hf(_hf), numLines(_numLines), ways(_ways), cands(_candidates)
{
    assert_msg(ways > 1, "zcaches need >=2 ways to work");
    assert_msg(cands >= ways, "candidates < ways does not make sense in a zcache");
    assert_msg(numLines % ways == 0, "number of lines is not a multiple of ways");

    //Populate secondary parameters
    numSets = numLines/ways;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
    setMask = numSets - 1;

    lookupArray = gm_calloc<uint32_t>(numLines);
    array = gm_calloc<Address>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        lookupArray[i] = i;  // start with a linear mapping; with swaps, it'll get progressively scrambled
    }
    swapArray = gm_calloc<uint32_t>(cands/ways + 2);  // conservative upper bound (tight within 2 ways)
}

void ZArray::initStats(AggregateStat* parentStat) {
    AggregateStat* objStats = new AggregateStat();
    objStats->init("array", "ZArray stats");
    statSwaps.init("swaps", "Block swaps in replacement process");
    objStats->append(&statSwaps);
    parentStat->append(objStats);
}

int32_t ZArray::lookup(const Address lineAddr, const MemReq* req, bool updateReplacement) {
    /* Be defensive: If the line is 0, panic instead of asserting. Now this can
     * only happen on a segfault in the main program, but when we move to full
     * system, phy page 0 might be used, and this will hit us in a very subtle
     * way if we don't check.
     */
    if (unlikely(!lineAddr)) panic("ZArray::lookup called with lineAddr==0 -- your app just segfaulted");

    for (uint32_t w = 0; w < ways; w++) {
        uint32_t lineId = lookupArray[w*numSets + (hf->hash(w, lineAddr) & setMask)];
        if (array[lineId] == lineAddr) {
            if (updateReplacement) {
                rp->update(lineId, req);
            }
            return lineId;
        }
    }
    return -1;
}

uint32_t ZArray::preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr) {
    ZWalkInfo candidates[cands + ways]; //extra ways entries to avoid checking on every expansion

    bool all_valid = true;
    uint32_t fringeStart = 0;
    uint32_t numCandidates = ways; //seeds

    //info("Replacement for incoming 0x%lx", lineAddr);

    //Seeds
    for (uint32_t w = 0; w < ways; w++) {
        uint32_t pos = w*numSets + (hf->hash(w, lineAddr) & setMask);
        uint32_t lineId = lookupArray[pos];
        candidates[w].set(pos, lineId, -1);
        all_valid &= (array[lineId] != 0);
        //info("Seed Candidate %d addr 0x%lx pos %d lineId %d", w, array[lineId], pos, lineId);
    }

    //Expand fringe in BFS fashion
    while (numCandidates < cands && all_valid) {
        uint32_t fringeId = candidates[fringeStart].lineId;
        Address fringeAddr = array[fringeId];
        assert(fringeAddr);
        for (uint32_t w = 0; w < ways; w++) {
            uint32_t hval = hf->hash(w, fringeAddr) & setMask;
            uint32_t pos = w*numSets + hval;
            uint32_t lineId = lookupArray[pos];

            // Logically, you want to do this...
#if 0
            if (lineId != fringeId) {
                //info("Candidate %d way %d addr 0x%lx pos %d lineId %d parent %d", numCandidates, w, array[lineId], pos, lineId, fringeStart);
                candidates[numCandidates++].set(pos, lineId, (int32_t)fringeStart);
                all_valid &= (array[lineId] != 0);
            }
#endif
            // But this compiles as a branch and ILP sucks (this data-dependent branch is long-latency and mispredicted often)
            // Logically though, this is just checking for whether we're revisiting ourselves, so we can eliminate the branch as follows:
            candidates[numCandidates].set(pos, lineId, (int32_t)fringeStart);
            all_valid &= (array[lineId] != 0);  // no problem, if lineId == fringeId the line's already valid, so no harm done
            numCandidates += (lineId != fringeId); // if lineId == fringeId, the cand we just wrote will be overwritten
        }
        fringeStart++;
    }

    //Get best candidate (NOTE: This could be folded in the code above, but it's messy since we can expand more than zassoc elements)
    assert(!all_valid || numCandidates >= cands);
    numCandidates = (numCandidates > cands)? cands : numCandidates;

    //info("Using %d candidates, all_valid=%d", numCandidates, all_valid);

    uint32_t bestCandidate = rp->rankCands(req, ZCands(&candidates[0], &candidates[numCandidates]));
    assert(bestCandidate < numLines);

    //Fill in swap array

    //Get the *minimum* index of cands that matches lineId. We need the minimum in case there are loops (rare, but possible)
    uint32_t minIdx = -1;
    for (uint32_t ii = 0; ii < numCandidates; ii++) {
        if (bestCandidate == candidates[ii].lineId) {
            minIdx = ii;
            break;
        }
    }
    assert(minIdx >= 0);
    //info("Best candidate is %d lineId %d", minIdx, bestCandidate);

    lastCandIdx = minIdx; //used by timing simulation code to schedule array accesses

    int32_t idx = minIdx;
    uint32_t swapIdx = 0;
    while (idx >= 0) {
        swapArray[swapIdx++] = candidates[idx].pos;
        idx = candidates[idx].parentIdx;
    }
    swapArrayLen = swapIdx;
    assert(swapArrayLen > 0);

    //Write address of line we're replacing
    *wbLineAddr = array[bestCandidate];

    return bestCandidate;
}

void ZArray::postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate) {
    //We do the swaps in lookupArray, the array stays the same
    assert(lookupArray[swapArray[0]] == candidate);
    for (uint32_t i = 0; i < swapArrayLen-1; i++) {
        //info("Moving position %d (lineId %d) <- %d (lineId %d)", swapArray[i], lookupArray[swapArray[i]], swapArray[i+1], lookupArray[swapArray[i+1]]);
        lookupArray[swapArray[i]] = lookupArray[swapArray[i+1]];
    }
    lookupArray[swapArray[swapArrayLen-1]] = candidate; //note that in preinsert() we walk the array backwards when populating swapArray, so the last elem is where the new line goes
    //info("Inserting lineId %d in position %d", candidate, swapArray[swapArrayLen-1]);

    rp->replaced(candidate);
    array[candidate] = lineAddr;
    rp->update(candidate, req);

    statSwaps.inc(swapArrayLen-1);
}

