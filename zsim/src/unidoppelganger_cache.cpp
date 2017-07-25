#include "unidoppelganger_cache.h"
#include "pin.H"

#include <cstdlib>
#include <time.h>

uniDoppelgangerCache::uniDoppelgangerCache(uint32_t _numLines, CC* _cc, uniDoppelgangerTagArray* _tagArray, 
uniDoppelgangerDataArray* _dataArray, CacheArray* _array, ReplPolicy* tagRP, ReplPolicy* dataRP, uint32_t _accLat, uint32_t _invLat, 
uint32_t mshrs, uint32_t tagLat, uint32_t mtagLat, uint32_t mapLat, uint32_t dataLat, uint32_t ways, uint32_t cands, 
uint32_t _domain, const g_string& _name) : TimingCache(_numLines, _cc, _array, tagRP, _accLat, _invLat, mshrs, tagLat, 
ways, cands, _domain, _name), tagArray(_tagArray), dataArray(_dataArray), tagLat(tagLat), mtagLat(mtagLat), 
mapLat(mapLat), dataLat(dataLat), tagRP(tagRP), dataRP(dataRP) {}

void uniDoppelgangerCache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    tagArray->initStats(cacheStat);
    dataArray->initStats(cacheStat);
    tagRP->initStats(cacheStat);
    dataRP->initStats(cacheStat);
}

uint64_t uniDoppelgangerCache::access(MemReq& req) {
    // // info("%lu: REQ %s to address %lu.", req.cycle, AccessTypeName(req.type), req.lineAddr);
    DataLine data = gm_calloc<uint8_t>(zinfo->lineSize);
    DataType type = ZSIM_FLOAT;
    DataValue min, max;
    bool approximate = false;
    for(uint32_t i = 0; i < zinfo->approximateRegions.size(); i++) {
        if ((req.lineAddr << lineBits) > std::get<0>(zinfo->approximateRegions[i]) && (req.lineAddr << lineBits) < std::get<1>(zinfo->approximateRegions[i])) {
            type = std::get<2>(zinfo->approximateRegions[i]);
            min = std::get<3>(zinfo->approximateRegions[i]);
            max = std::get<4>(zinfo->approximateRegions[i]);
            approximate = true;
            break;
        }
    }
    if (approximate)
        PIN_SafeCopy(data, (void*)(req.lineAddr << lineBits), zinfo->lineSize);
    
    EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
    assert_msg(evRec, "DoppelgangerCache is not connected to TimingCore");

    // Tie two events to an optional timing record
    // TODO: Promote to evRec if this is more generally useful
    auto connect = [evRec](const TimingRecord* r, TimingEvent* startEv, TimingEvent* endEv, uint64_t startCycle, uint64_t endCycle) {
        assert_msg(startCycle <= endCycle, "start > end? %ld %ld", startCycle, endCycle);
        if (r) {
            assert_msg(startCycle <= r->reqCycle, "%ld / %ld", startCycle, r->reqCycle);
            assert_msg(r->respCycle <= endCycle, "%ld %ld %ld %ld", startCycle, r->reqCycle, r->respCycle, endCycle);
            uint64_t upLat = r->reqCycle - startCycle;
            uint64_t downLat = endCycle - r->respCycle;

            if (upLat) {
                DelayEvent* dUp = new (evRec) DelayEvent(upLat);
                dUp->setMinStartCycle(startCycle);
                startEv->addChild(dUp, evRec)->addChild(r->startEvent, evRec);
            } else {
                startEv->addChild(r->startEvent, evRec);
            }

            if (downLat) {
                DelayEvent* dDown = new (evRec) DelayEvent(downLat);
                dDown->setMinStartCycle(r->respCycle);
                r->endEvent->addChild(dDown, evRec)->addChild(endEv, evRec);
            } else {
                r->endEvent->addChild(endEv, evRec);
            }
        } else {
            if (startCycle == endCycle) {
                startEv->addChild(endEv, evRec);
            } else {
                DelayEvent* dEv = new (evRec) DelayEvent(endCycle - startCycle);
                dEv->setMinStartCycle(startCycle);
                startEv->addChild(dEv, evRec)->addChild(endEv, evRec);
            }
        }
    };

    TimingRecord tagWritebackRecord, accessRecord, tr;
    tagWritebackRecord.clear();
    accessRecord.clear();
    g_vector<TimingRecord> writebackRecords;
    g_vector<uint64_t> wbStartCycles;
    g_vector<uint64_t> wbEndCycles;
    uint64_t tagEvDoneCycle = 0;
    uint64_t respCycle = req.cycle;
    uint64_t evictCycle = req.cycle;

    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        // info("%lu: REQ %s to address %lu confirmed approximate.", req.cycle, AccessTypeName(req.type), req.lineAddr << lineBits);
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t tagId = tagArray->lookup(req.lineAddr, &req, updateReplacement);

        MissStartEvent* mse;
        MissResponseEvent* mre;
        DelayEvent* del;
        MissWritebackEvent* mwe;
        if (tagId == -1) {
            if (approximate) {
                // info("\tExact Miss Req");
                // Exact line, free a data line and a tag for it.
                respCycle += tagLat;
                evictCycle += tagLat;
                assert(cc->shouldAllocate(req));

                // Get the eviction candidate
                Address wbLineAddr;
                int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

                // Need to evict the tag.
                // info("\t\tEvicting tagId: %i", victimTagId);
                evictCycle += MAX(mtagLat, dataLat);
                tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
                int32_t newLLHead;
                bool approximateVictim;
                bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead, &approximateVictim);
                int32_t victimDataId = tagArray->readMapId(victimTagId);
                if (evictDataLine) {
                    // info("\t\tAlong with dataId: %i", victimDataId);
                    // Clear (Evict, Tags already evicted) data line
                    dataArray->postinsert(-1, &req, victimDataId, -1, false, false);
                } else if (newLLHead != -1) {
                    // Change Tag
                    uint32_t victimMap = dataArray->readMap(victimDataId);
                    dataArray->postinsert(victimMap, &req, victimDataId, newLLHead, approximateVictim, false);
                }
                if (evRec->hasRecord()) {
                    tagWritebackRecord.clear();
                    tagWritebackRecord = evRec->popRecord();
                }
                uint32_t map = dataArray->calculateMap(data, type, min, max);
                // // info("\tMiss Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
                // info("\tMiss Req map: %u", map);
                int32_t mapId = dataArray->lookup(map, &req, updateReplacement);

                // Need to get the line we want
                uint64_t getDoneCycle = respCycle;
                respCycle = cc->processAccess(req, victimTagId, respCycle, &getDoneCycle);
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                
                // evictCycle = respCycle + mapLat + mtagLat;
                if (mapId != -1) {
                    // info("\tSimilar map at: %i", mapId);
                    // Found similar mtag, insert tag to the LL.
                    int32_t oldListHead = dataArray->readListHead(mapId);
                    tagArray->postinsert(req.lineAddr, &req, victimTagId, mapId, oldListHead, true, updateReplacement);
                    dataArray->postinsert(map, &req, mapId, victimTagId, true, updateReplacement);

                    assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);

                    mse = new (evRec) MissStartEvent(this, tagLat, domain);
                    mre = new (evRec) MissResponseEvent(this, mse, domain);
                    del = new (evRec) DelayEvent(2*tagLat + mapLat + mtagLat);
                    mwe = new (evRec) MissWritebackEvent(this, mse, evictCycle - req.cycle, domain);

                    mse->setMinStartCycle(req.cycle);
                    // info("\t\t\tMiss Start Event: %lu, %u", req.cycle, tagLat);
                    mre->setMinStartCycle(respCycle);
                    // info("\t\t\tMiss Response Event: %lu", respCycle);
                    del->setMinStartCycle(respCycle);
                    // info("\t\t\tMiss Delay Event: %lu, %u", respCycle, 2*tagLat + mapLat + mtagLat);
                    mwe->setMinStartCycle(MAX(respCycle + 2*tagLat + mapLat + mtagLat, tagEvDoneCycle));
                    // info("\t\t\tMiss writeback event: %lu, %lu", MAX(respCycle + 2*tagLat + mapLat + mtagLat, tagEvDoneCycle), evictCycle - req.cycle);

                    connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + tagLat, respCycle);
                    mre->addChild(del, evRec)->addChild(mwe, evRec);
                    if (tagEvDoneCycle) {
                        connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, mse, mwe, req.cycle + tagLat, tagEvDoneCycle);
                    }
                } else {
                    // info("\tNo similar map");
                    // allocate new data/mtag and evict another if necessary, 
                    // evict the tags associated with it too.
                    evictCycle = respCycle + mapLat + mtagLat;
                    int32_t victimListHeadId;
                    int32_t victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                    uint64_t evBeginCycle = evictCycle;
                    TimingRecord writebackRecord;

                    while (victimListHeadId != -1) {
                        Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                        // info("\t\tEvicting tagId: %i", victimListHeadId);
                        uint64_t evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                        tagArray->postinsert(-1, &req, victimListHeadId, -1, -1, false, false);
                        victimListHeadId = tagArray->readNextLL(victimListHeadId);
                        if (evRec->hasRecord()) {
                            writebackRecord.clear();
                            writebackRecord = evRec->popRecord();
                            writebackRecords.push_back(writebackRecord);
                            wbStartCycles.push_back(evBeginCycle);
                            wbEndCycles.push_back(evDoneCycle);
                            evBeginCycle += tagLat;
                        }
                    }
                    tagArray->postinsert(req.lineAddr, &req, victimTagId, victimDataId, -1, true, updateReplacement);
                    dataArray->postinsert(map, &req, victimDataId, victimTagId, true, updateReplacement);
                    assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);

                    mse = new (evRec) MissStartEvent(this, tagLat, domain);
                    mre = new (evRec) MissResponseEvent(this, mse, domain);
                    del = new (evRec) DelayEvent(evBeginCycle - evictCycle);
                    mwe = new (evRec) MissWritebackEvent(this, mse, tagLat + MAX(mtagLat, dataLat), domain);

                    mse->setMinStartCycle(req.cycle);
                    // info("\t\t\tMiss Start Event: %lu, %u", req.cycle, tagLat);
                    mre->setMinStartCycle(respCycle);
                    // info("\t\t\tMiss Response Event: %lu", respCycle);
                    del->setMinStartCycle(evictCycle);
                    // info("\t\t\tMiss Delay Event: %lu, %lu", evictCycle, evBeginCycle - evictCycle);
                    mwe->setMinStartCycle(MAX(respCycle + evBeginCycle - evictCycle, tagEvDoneCycle));
                    // info("\t\t\tMiss writeback event: %lu, %u", MAX(respCycle + evBeginCycle - evictCycle, tagEvDoneCycle), tagLat + MAX(mtagLat, dataLat));

                    connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + tagLat, respCycle);
                    for(uint32_t i = 0; i < wbStartCycles.size(); i++)
                        connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, mre, mwe, wbStartCycles[i], wbEndCycles[i]);
                    mre->addChild(del, evRec)->addChild(mwe, evRec);
                    if (tagEvDoneCycle) {
                        connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, mse, mwe, req.cycle + tagLat, tagEvDoneCycle);
                    }
                }
            } else {
                // Miss, no tags found.
                // info("\tMiss Req");
                respCycle += tagLat;
                evictCycle += tagLat;
                assert(cc->shouldAllocate(req));

                // Get the eviction candidate
                Address wbLineAddr;
                int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

                // Need to evict the tag.
                // info("\t\tEvicting tagId: %i", victimTagId);
                evictCycle += MAX(mtagLat, dataLat);
                tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
                int32_t newLLHead;
                bool approximateVictim;
                bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead, &approximateVictim);
                int32_t victimDataId = tagArray->readMapId(victimTagId);
                if (evictDataLine) {
                    // info("\t\tAlong with dataId: %i", victimDataId);
                    // Clear (Evict, Tags already evicted) data line
                    dataArray->postinsert(-1, &req, victimDataId, -1, false, false);
                } else if (newLLHead != -1) {
                    // Change Tag
                    uint32_t victimMap = dataArray->readMap(victimDataId);
                    dataArray->postinsert(victimMap, &req, victimDataId, newLLHead, approximateVictim, false);
                }
                if (evRec->hasRecord()) {
                    tagWritebackRecord.clear();
                    tagWritebackRecord = evRec->popRecord();
                }
                // Need to get the line we want
                uint64_t getDoneCycle = respCycle;
                respCycle = cc->processAccess(req, victimTagId, respCycle, &getDoneCycle);
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();

                // Need to free a data line
                srand (time(NULL));
                int32_t victimListHeadId;
                uint32_t map = rand() % (uint32_t)std::pow(2, zinfo->mapSize-1);
                victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                evictCycle += MAX(mtagLat, dataLat);
                uint64_t evBeginCycle = evictCycle;
                TimingRecord writebackRecord;

                while (victimListHeadId != -1) {
                    Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                    // info("\t\tEvicting tagId: %i", victimListHeadId);
                    uint64_t evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                    tagArray->postinsert(-1, &req, victimListHeadId, -1, -1, false, false);
                    victimListHeadId = tagArray->readNextLL(victimListHeadId);
                    if (evRec->hasRecord()) {
                        writebackRecord.clear();
                        writebackRecord = evRec->popRecord();
                        writebackRecords.push_back(writebackRecord);
                        wbStartCycles.push_back(evBeginCycle);
                        wbEndCycles.push_back(evDoneCycle);
                        evBeginCycle += tagLat;
                    }
                }
                tagArray->postinsert(req.lineAddr, &req, victimTagId, victimDataId, -1, false, updateReplacement);
                dataArray->postinsert(-1, &req, victimDataId, victimTagId, false, updateReplacement);

                assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);
                mse = new (evRec) MissStartEvent(this, tagLat, domain);
                mre = new (evRec) MissResponseEvent(this, mse, domain);
                mwe = new (evRec) MissWritebackEvent(this, mse, tagLat + dataLat, domain);

                mse->setMinStartCycle(req.cycle);
                // info("\t\t\tMiss Start Event: %lu, %u", req.cycle, tagLat);
                mre->setMinStartCycle(respCycle);
                // info("\t\t\tMiss Response Event: %lu", respCycle);
                mwe->setMinStartCycle(MAX(evBeginCycle, tagEvDoneCycle));
                // info("\t\t\tMiss writeback event: %lu, %u", MAX(respCycle + evBeginCycle - evictCycle, tagEvDoneCycle), tagLat + MAX(mtagLat, dataLat));

                connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + tagLat, respCycle);
                for(uint32_t i = 0; i < wbStartCycles.size(); i++)
                    connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, mse, mwe, wbStartCycles[i], wbEndCycles[i]);
                mre->addChild(mwe, evRec);
                if (tagEvDoneCycle) {
                    connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, mse, mwe, req.cycle + tagLat, tagEvDoneCycle);
                }
            }
            tr.startEvent = mse;
            tr.endEvent = mre;
        } else {
            if (approximate && req.type == PUTX) {
                // If this is a write
                uint32_t map = dataArray->calculateMap(data, type, min, max);
                respCycle += MAX(tagLat,mapLat);
                // // info("\tHit Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
                // info("\tHit PUTX Req map: %u, tagId = %i", map, tagId);
                uint32_t previousMap = dataArray->readMap(tagArray->readMapId(tagId));
                respCycle += MAX(mtagLat, dataLat);

                if (map == previousMap) {
                    // info("\tMap not changed");
                    // and its map is the same as before. Do nothing.
                    uint64_t getDoneCycle = respCycle;
                    respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                    dataArray->lookup(map, &req, updateReplacement);
                    HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                    // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                    ev->setMinStartCycle(req.cycle);
                    tr.startEvent = tr.endEvent = ev;
                } else {
                    // and its map is not the same as before.
                    int32_t mapId = dataArray->lookup(map, &req, updateReplacement);
                    if (mapId != -1) {
                        // info("\tSimilar map at: %i", mapId);
                        // but is similar to something else that exists.
                        // we only need to add the tag to the existing linked
                        // list.
                        int32_t newLLHead;
                        bool approximateVictim;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead, &approximateVictim);
                        int32_t victimDataId = tagArray->readMapId(tagId);
                        if (evictDataLine) {
                            // info("\t\tEvicting dataId %i previously associated with tagId %i", victimDataId, tagId);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(-1, &req, victimDataId, -1, false, false);
                        } else if (newLLHead != -1) {
                            // Change Tag
                            uint32_t victimMap = dataArray->readMap(victimDataId);
                            dataArray->postinsert(victimMap, &req, victimDataId, newLLHead, approximateVictim, false);
                        }
                        int32_t oldListHead = dataArray->readListHead(mapId);
                        tagArray->postinsert(req.lineAddr, &req, tagId, mapId, oldListHead, true, false);
                        dataArray->postinsert(map, &req, mapId, tagId, true, false);
                        respCycle += MAX(2*tagLat, dataLat);

                        uint64_t getDoneCycle = respCycle;
                        respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                        if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                        tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                        HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                        // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                        ev->setMinStartCycle(req.cycle);
                        tr.startEvent = tr.endEvent = ev;
                    } else {
                        // info("\tNo similar map");
                        // and is also not similar to anything we have, we 
                        // need to allocate new data, and evict another if we
                        // have to.
                        int32_t victimListHeadId;
                        int32_t victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                        respCycle += tagLat;
                        uint64_t wbStartCycle = respCycle;
                        uint64_t evDoneCycle;
                        uint64_t firstWbCycle = respCycle;
                        TimingRecord writebackRecord;

                        while (victimListHeadId != -1) {
                            if (victimListHeadId != tagId) {
                                Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                                // info("\t\tEvicting tagId %i associated with victim dataId %i", victimListHeadId, victimDataId);
                                evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, wbStartCycle);
                                tagArray->postinsert(-1, &req, victimListHeadId, -1, -1, false, false);
                            }
                            victimListHeadId = tagArray->readNextLL(victimListHeadId);
                            if (evRec->hasRecord()) {
                                writebackRecord.clear();
                                writebackRecord = evRec->popRecord();
                                writebackRecords.push_back(writebackRecord);
                                wbStartCycles.push_back(wbStartCycle);
                                wbStartCycle += tagLat;
                                respCycle += tagLat;
                                wbEndCycles.push_back(evDoneCycle);
                            }
                        }

                        int32_t newLLHead;
                        bool approximateVictim;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead, &approximateVictim);
                        int32_t victimDataId2 = tagArray->readMapId(tagId);
                        if (evictDataLine) {
                            // info("\t\tClearing dataId %i previously associated with tagId %i", victimDataId2, tagId);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(-1, &req, victimDataId2, -1, false, false);
                        } else if (newLLHead != -1) {
                            // Change Tag
                            uint32_t victimMap = dataArray->readMap(victimDataId2);
                            dataArray->postinsert(victimMap, &req, victimDataId2, newLLHead,approximateVictim, false);
                        }
                        tagArray->postinsert(req.lineAddr, &req, tagId, victimDataId, -1, true, false);
                        dataArray->postinsert(map, &req, victimDataId, tagId, true, false);
                        respCycle += MAX(mtagLat, dataLat);

                        uint64_t getDoneCycle = respCycle;
                        respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                        if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                        tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};

                        HitEvent* he = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                        uHitWritebackEvent* hwe = new (evRec) uHitWritebackEvent(this, he, respCycle - req.cycle, domain);

                        he->setMinStartCycle(req.cycle);
                        hwe->setMinStartCycle(firstWbCycle);
                        // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                        // info("\t\t\tHit writeback Event: %lu, %lu", firstWbCycle, respCycle - firstWbCycle);

                        if(wbStartCycles.size())
                            for(uint32_t i = 0; i < wbStartCycles.size(); i++)
                                connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, he, hwe, wbStartCycles[i], wbEndCycles[i]);

                        tr.startEvent = tr.endEvent = he;
                    }
                }
            } else {
                // info("\tHit Req");
                dataArray->lookup(tagArray->readMapId(tagId), &req, updateReplacement);
                uint64_t getDoneCycle = respCycle;
                respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                ev->setMinStartCycle(req.cycle);
                tr.startEvent = tr.endEvent = ev;
            }
        }
        gm_free(data);
        evRec->pushRecord(tr);

        // tagArray->print();
        // dataArray->print();
    }

    cc->endAccess(req);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void uniDoppelgangerCache::simulateHitWriteback(uHitWritebackEvent* ev, uint64_t cycle, HitEvent* he) {
    uint64_t lookupCycle = tryLowPrioAccess(cycle);
    if (lookupCycle) { //success, release MSHR
        if (!pendingQueue.empty()) {
            //// info("XXX %ld elems in pending queue", pendingQueue.size());
            for (TimingEvent* qev : pendingQueue) {
                qev->requeue(cycle+1);
            }
            pendingQueue.clear();
        }
        ev->done(cycle);
    } else {
        ev->requeue(cycle+1);
    }
}