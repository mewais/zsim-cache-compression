#include "doppelganger_cache.h"
#include "event_recorder.h"
#include "timing_event.h"
#include <cstring>
#include "pin.H"

DoppelgangerCache::DoppelgangerCache(uint32_t _numLines, CC* _cc, CC* _dcc, DoppelgangerTagArray* _tagArray, DoppelgangerDataArray* _dataArray, 
CacheArray* _array, ReplPolicy* tagRP, ReplPolicy* dataRP, ReplPolicy* RP, uint32_t _accLat, uint32_t _invLat, 
uint32_t mshrs, uint32_t tagLat, uint32_t mtagLat, uint32_t mapLat, uint32_t dataLat, uint32_t ways, uint32_t cands,
uint32_t _domain, const g_string& _name) : TimingCache(_numLines, _cc, _array, RP, _accLat, _invLat, mshrs, tagLat, 
ways, cands, _domain, _name), dcc(_dcc), tagArray(_tagArray), dataArray(_dataArray), tagLat(tagLat), mtagLat(mtagLat), 
mapLat(mapLat), dataLat(dataLat), tagRP(tagRP), dataRP(dataRP) {}

void DoppelgangerCache::setParents(uint32_t childId, const g_vector<MemObject*>& parents, Network* network) {
    dcc->setParents(childId, parents, network);
    TimingCache::setParents(childId, parents, network);
}

void DoppelgangerCache::setChildren(const g_vector<BaseCache*>& children, Network* network) {
    dcc->setChildren(children, network);
    TimingCache::setChildren(children, network);
}

uint64_t DoppelgangerCache::access(MemReq& req) {
    // // info("%lu: REQ %s to address %lu.", req.cycle, AccessTypeName(req.type), req.lineAddr);
    DataLine data = gm_calloc<uint8_t>(zinfo->lineSize);
    DataType type;
    DataValue min, max;
    bool Found = false;
    for(uint32_t i = 0; i < zinfo->approximateRegions.size(); i++) {
        if ((req.lineAddr << lineBits) > std::get<0>(zinfo->approximateRegions[i]) && (req.lineAddr << lineBits) < std::get<1>(zinfo->approximateRegions[i])) {
            type = std::get<2>(zinfo->approximateRegions[i]);
            min = std::get<3>(zinfo->approximateRegions[i]);
            max = std::get<4>(zinfo->approximateRegions[i]);
            Found = true;
            break;
        }
    }
    if (!Found) {
        // // info("%lu: REQ %s to address %lu confirmed exact.", req.cycle, AccessTypeName(req.type), req.lineAddr << lineBits);
        return TimingCache::access(req);
    }
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

    bool skipAccess = dcc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        // info("%lu: REQ %s to address %lu confirmed approximate.", req.cycle, AccessTypeName(req.type), req.lineAddr << lineBits);
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t tagId = tagArray->lookup(req.lineAddr, &req, updateReplacement);

        if (tagId == -1) {
            // Miss, no tags found.
            // info("\tMiss Req");
            respCycle += tagLat;
            evictCycle += tagLat;
            assert(dcc->shouldAllocate(req));

            // Get the eviction candidate
            Address wbLineAddr;
            int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace

            trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

            // Need to evict the tag.
            // info("\t\tEvicting tagId: %i", victimTagId);
            evictCycle += MAX(mtagLat, dataLat);
            tagEvDoneCycle = dcc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
            int32_t newLLHead;
            bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead);
            int32_t victimDataId = tagArray->readMapId(victimTagId);
            if (evictDataLine) {
                // info("\t\tAlong with dataId: %i", victimDataId);
                // Clear (Evict, Tags already evicted) data line
                dataArray->postinsert(0, &req, victimDataId, -1, false);
            } else if (newLLHead != -1) {
                // Change Tag
                uint32_t victimMap = dataArray->readMap(victimDataId);
                dataArray->postinsert(victimMap, &req, victimDataId, newLLHead, false);
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
            respCycle = dcc->processAccess(req, victimTagId, respCycle, &getDoneCycle);
            tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
            if (evRec->hasRecord()) accessRecord = evRec->popRecord();
            
            // evictCycle = respCycle + mapLat + mtagLat;
            MissStartEvent* mse;
            MissResponseEvent* mre;
            DelayEvent* del;
            MissWritebackEvent* mwe;
            if (mapId != -1) {
                // info("\tSimilar map at: %i", mapId);
                // Found similar mtag, insert tag to the LL.
                int32_t oldListHead = dataArray->readListHead(mapId);
                tagArray->postinsert(req.lineAddr, &req, victimTagId, mapId, oldListHead, updateReplacement);
                dataArray->postinsert(map, &req, mapId, victimTagId, updateReplacement);

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
                    uint64_t evDoneCycle = dcc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                    tagArray->postinsert(0, &req, victimListHeadId, -1, -1, false);
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
                tagArray->postinsert(req.lineAddr, &req, victimTagId, victimDataId, -1, updateReplacement);
                dataArray->postinsert(map, &req, victimDataId, victimTagId, updateReplacement);
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
            tr.startEvent = mse;
            tr.endEvent = mre;
        } else {
            if (req.type == PUTX) {
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
                    respCycle = dcc->processAccess(req, tagId, respCycle, &getDoneCycle);
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
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead);
                        int32_t victimDataId = tagArray->readMapId(tagId);
                        if (evictDataLine) {
                            // info("\t\tEvicting dataId %i previously associated with tagId %i", victimDataId, tagId);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(0, &req, victimDataId, -1, false);
                        } else if (newLLHead != -1) {
                            // Change Tag
                            uint32_t victimMap = dataArray->readMap(victimDataId);
                            dataArray->postinsert(victimMap, &req, victimDataId, newLLHead, false);
                        }
                        int32_t oldListHead = dataArray->readListHead(mapId);
                        tagArray->postinsert(req.lineAddr, &req, tagId, mapId, oldListHead, false);
                        dataArray->postinsert(map, &req, mapId, tagId, false);
                        respCycle += MAX(2*tagLat, dataLat);

                        uint64_t getDoneCycle = respCycle;
                        respCycle = dcc->processAccess(req, tagId, respCycle, &getDoneCycle);
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
                                evDoneCycle = dcc->processEviction(req, wbLineAddr, victimListHeadId, wbStartCycle);
                                tagArray->postinsert(0, &req, victimListHeadId, -1, -1, false);
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
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead);
                        int32_t victimDataId2 = tagArray->readMapId(tagId);
                        if (evictDataLine) {
                            // info("\t\tClearing dataId %i previously associated with tagId %i", victimDataId2, tagId);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(0, &req, victimDataId2, -1, false);
                        } else if (newLLHead != -1) {
                            // Change Tag
                            uint32_t victimMap = dataArray->readMap(victimDataId2);
                            dataArray->postinsert(victimMap, &req, victimDataId2, newLLHead, false);
                        }
                        tagArray->postinsert(req.lineAddr, &req, tagId, victimDataId, -1, false);
                        dataArray->postinsert(map, &req, victimDataId, tagId, false);
                        respCycle += MAX(mtagLat, dataLat);

                        uint64_t getDoneCycle = respCycle;
                        respCycle = dcc->processAccess(req, tagId, respCycle, &getDoneCycle);
                        if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                        tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};

                        HitEvent* he = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                        HitWritebackEvent* hwe = new (evRec) HitWritebackEvent(this, he, respCycle - req.cycle, domain);

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
                respCycle = dcc->processAccess(req, tagId, respCycle, &getDoneCycle);
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

    dcc->endAccess(req);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void DoppelgangerCache::initCacheStats(AggregateStat* cacheStat) {
    tagArray->initStats(cacheStat);
    dataArray->initStats(cacheStat);
    tagRP->initStats(cacheStat);
    dataRP->initStats(cacheStat);
    dcc->initStats(cacheStat);
    TimingCache::initCacheStats(cacheStat);
}

uint64_t DoppelgangerCache::invalidate(const InvReq& req) {
    bool Found = false;
    for(uint32_t i = 0; i < zinfo->approximateRegions.size(); i++) {
        if ((req.lineAddr << lineBits) > std::get<0>(zinfo->approximateRegions[i]) && (req.lineAddr << lineBits) < std::get<1>(zinfo->approximateRegions[i])) {
            Found = true;
            break;
        }
    }
    if (Found) {
        // info("%lu: INV to approximate address %lu.", req.cycle, req.lineAddr << lineBits);
        startInvalidate(req);
        return finishInvalidate(req);
    } else {
        TimingCache::startInvalidate();
        return TimingCache::finishInvalidate(req);
    }
}

void DoppelgangerCache::startInvalidate(const InvReq& req) {
    dcc->startInv(); //note we don't grab tcc; tcc serializes multiple up accesses, down accesses don't see it
}

uint64_t DoppelgangerCache::finishInvalidate(const InvReq& req) {
    // TODO: This is OK for now because we are the sole LLC, no one will invalidate us. We need to handle accesses here later.
    int32_t lineId = array->lookup(req.lineAddr, nullptr, false);
    assert_msg(lineId != -1, "[%s] Invalidate on non-existing address 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    uint64_t respCycle = req.cycle + invLat;
    trace(Cache, "[%s] Invalidate start 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    respCycle = dcc->processInv(req, lineId, respCycle); //send invalidates or downgrades to children, and adjust our own state
    trace(Cache, "[%s] Invalidate end 0x%lx type %s lineId %d, reqWriteback %d, latency %ld", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback, respCycle - req.cycle);

    return respCycle;
}

void DoppelgangerCache::simulateHitWriteback(HitWritebackEvent* ev, uint64_t cycle, HitEvent* he) {
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