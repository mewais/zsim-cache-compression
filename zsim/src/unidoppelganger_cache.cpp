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

    TimingRecord writebackRecord, accessRecord;
    writebackRecord.clear();
    accessRecord.clear();
    g_vector<TimingRecord> writebackRecords;
    writebackRecords.push_back(writebackRecord);
    uint64_t evDoneCycle = 0;

    uint64_t respCycle = req.cycle;
    bool Hit;
    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)

    if (likely(!skipAccess)) {
        // info("%lu: REQ %s to address %lu confirmed approximate.", req.cycle, AccessTypeName(req.type), req.lineAddr << lineBits);
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t tagId = tagArray->lookup(req.lineAddr, &req, updateReplacement);
        respCycle += accLat;

        if (tagId == -1) {
            Hit = false;
            assert(cc->shouldAllocate(req));

            if (approximate) {
                // If this is approximate, do like Doppelganger
                uint32_t map = dataArray->calculateMap(data, type, min, max);

                // // info("\tMiss Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
                // info("\tApproximate Miss Req map: %u", map);
                int32_t mapId = dataArray->lookup(map, &req, updateReplacement);
                if (mapId != -1) {
                    // info("\tSimilar map at: %i", mapId);
                    // Found similar mtag, allocate new tag and evict another if necessary.
                    Address wbLineAddr;
                    int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                    tagId = victimTagId;
                    trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);
                    // info("\t\tEvicting tagId: %i", victimTagId);
                    evDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, respCycle);

                    int32_t newLLHead;
                    bool approximateVictim;
                    bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead, &approximateVictim);
                    int32_t victimDataId = tagArray->readMapId(victimTagId);
                    if (evictDataLine) {
                        // This will evect the data line, whether exact or approximate
                        // info("\t\tAlong with dataId: %i", victimDataId);
                        // Clear (Evict, Tags already evicted) data line
                        dataArray->postinsert(-1, &req, victimDataId, -1, false, false);
                    } else if (newLLHead != -1) {
                        // Change Tag
                        uint32_t victimMap = dataArray->readMap(victimDataId);
                        dataArray->postinsert(victimMap, &req, victimDataId, newLLHead, approximateVictim, false);
                    }
                    int32_t oldListHead = dataArray->readListHead(mapId);
                    tagArray->postinsert(req.lineAddr, &req, victimTagId, mapId, oldListHead, true, updateReplacement);
                    dataArray->postinsert(map, &req, mapId, victimTagId, true, updateReplacement);
                    
                    if (evRec->hasRecord()) {
                        writebackRecord.clear();
                        writebackRecord = evRec->popRecord();
                        writebackRecords.push_back(writebackRecord);
                    }
                } else {
                    // info("\tNo similar map");
                    // couldn't find a similar mtag, allocate new data/mtag and evict another
                    // if necessary, evict the tags associated with it too.
                    int32_t victimListHeadId;
                    int32_t victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                    evDoneCycle = respCycle;

                    while (victimListHeadId != -1) {
                        Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                        // info("\t\tEvicting tagId: %i", victimListHeadId);
                        evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evDoneCycle);
                        tagArray->postinsert(0, &req, victimListHeadId, -1, -1, false, false);
                        victimListHeadId = tagArray->readNextLL(victimListHeadId);
                        if (evRec->hasRecord()) {
                            writebackRecord.clear();
                            writebackRecord = evRec->popRecord();
                            writebackRecords.push_back(writebackRecord);
                        }
                    }

                    Address wbLineAddr;
                    int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                    tagId = victimTagId;
                    trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

                    // info("\t\tEvicting tagId: %i", victimTagId);
                    evDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evDoneCycle); //if needed, send invalidates/downgrades to lower level, and wb to upper level
                    
                    int32_t newLLHead;
                    bool approximateVictim;
                    bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead, &approximateVictim);
                    int32_t victimDataId2 = tagArray->readMapId(victimTagId);
                    if (evictDataLine) {
                        // info("\t\tAlong with dataId: %i", victimDataId2);
                        // Clear (Evict, Tags already evicted) data line
                        dataArray->postinsert(0, &req, victimDataId2, -1, false, false);
                    } else if (newLLHead != -1) {
                        // Change Tag
                        uint32_t victimMap = dataArray->readMap(victimDataId2);
                        dataArray->postinsert(victimMap, &req, victimDataId2, newLLHead, approximateVictim, false);
                    }
                    tagArray->postinsert(req.lineAddr, &req, victimTagId, victimDataId, -1, true, updateReplacement);
                    dataArray->postinsert(map, &req, victimDataId, victimTagId, true, updateReplacement);

                    if (evRec->hasRecord()) {
                        writebackRecord.clear();
                        writebackRecord = evRec->popRecord();
                        writebackRecords.push_back(writebackRecord);
                    }
                }
            } else {
                // info("\tExact Miss Req");
                // Exact line, free a data line and a tag for it.
                srand (time(NULL));
                int32_t victimListHeadId;
                uint32_t map = rand() % (uint32_t)std::pow(2, zinfo->mapSize-1);
                int32_t victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                evDoneCycle = respCycle;

                while (victimListHeadId != -1) {
                    Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                    // info("\t\tEvicting tagId: %i", victimListHeadId);
                    evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evDoneCycle);
                    tagArray->postinsert(0, &req, victimListHeadId, -1, -1, false, false);
                    victimListHeadId = tagArray->readNextLL(victimListHeadId);
                    if (evRec->hasRecord()) {
                        writebackRecord.clear();
                        writebackRecord = evRec->popRecord();
                        writebackRecords.push_back(writebackRecord);
                    }
                }

                Address wbLineAddr;
                int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                tagId = victimTagId;
                trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

                // info("\t\tEvicting tagId: %i", victimTagId);
                evDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evDoneCycle); //if needed, send invalidates/downgrades to lower level, and wb to upper level
                
                int32_t newLLHead;
                bool approximateVictim;
                bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead, &approximateVictim);
                int32_t victimDataId2 = tagArray->readMapId(victimTagId);
                if (evictDataLine) {
                    // info("\t\tAlong with dataId: %i", victimDataId2);
                    // Clear (Evict, Tags already evicted) data line
                    dataArray->postinsert(0, &req, victimDataId2, -1, false, false);
                } else if (newLLHead != -1) {
                    // Change Tag
                    uint32_t victimMap = dataArray->readMap(victimDataId2);
                    dataArray->postinsert(victimMap, &req, victimDataId2, newLLHead, approximateVictim, false);
                }
                tagArray->postinsert(req.lineAddr, &req, victimTagId, victimDataId, -1, false, updateReplacement);
                dataArray->postinsert(-1, &req, victimDataId, victimTagId, false, updateReplacement);

                if (evRec->hasRecord()) {
                    writebackRecord.clear();
                    writebackRecord = evRec->popRecord();
                    writebackRecords.push_back(writebackRecord);
                }
            }
        } else {
            // This is a hit
            Hit = true;
            if (approximate && req.type == PUTX) {
                // If this is a write
                uint32_t map = dataArray->calculateMap(data, type, min, max);
                // // info("\tHit Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
                // info("\tApproximate Hit PUTX Req map: %u, tagId = %i", map, tagId);
                // respCycle += mapLat;
                uint32_t previousMap = dataArray->readMap(tagArray->readMapId(tagId));

                if (map == previousMap) {
                    // info("\tMap not changed");
                    // and its map is the same as before. Do nothing.
                    dataArray->lookup(map, &req, updateReplacement);
                    // respCycle += mtagLat + dataLat;
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
                            dataArray->postinsert(0, &req, victimDataId, -1, false, false);
                        } else if (newLLHead != -1) {
                            // Change Tag
                            uint32_t victimMap = dataArray->readMap(victimDataId);
                            dataArray->postinsert(victimMap, &req, victimDataId, newLLHead, approximateVictim, false);
                        }
                        int32_t oldListHead = dataArray->readListHead(mapId);
                        tagArray->postinsert(req.lineAddr, &req, tagId, mapId, oldListHead, true, false);
                        dataArray->postinsert(map, &req, mapId, tagId, true, false);
                        // respCycle += MAX(mtagLat, dataLat);
                    } else {
                        // info("\tNo similar map");
                        // and is also not similar to anything we have, we 
                        // need to allocate new data, and evict another if we
                        // have to.
                        int32_t victimListHeadId;
                        int32_t victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                        evDoneCycle = respCycle;

                        while (victimListHeadId != -1) {
                            if (victimListHeadId != tagId) {
                                Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                                // info("\t\tEvicting tagId %i associated with victim dataId %i", victimListHeadId, victimDataId);
                                evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evDoneCycle);
                                tagArray->postinsert(0, &req, victimListHeadId, -1, -1, false, false);
                            }
                            victimListHeadId = tagArray->readNextLL(victimListHeadId);
                            if (evRec->hasRecord()) {
                                writebackRecord.clear();
                                writebackRecord = evRec->popRecord();
                                writebackRecords.push_back(writebackRecord);
                            }
                        }

                        int32_t newLLHead;
                        bool approximateVictim;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead, &approximateVictim);
                        int32_t victimDataId2 = tagArray->readMapId(tagId);
                        if (evictDataLine) {
                            // info("\t\tEvicting dataId %i previously associated with tagId %i", victimDataId2, tagId);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(0, &req, victimDataId2, -1, false, false);
                        } else if (newLLHead != -1) {
                            // Change Tag
                            uint32_t victimMap = dataArray->readMap(victimDataId2);
                            dataArray->postinsert(victimMap, &req, victimDataId2, newLLHead, approximateVictim, false);
                        }
                        tagArray->postinsert(req.lineAddr, &req, tagId, victimDataId, -1, true, false);
                        dataArray->postinsert(map, &req, victimDataId, tagId, true, false);
                        // respCycle += mtagLat;
                    }
                }
            } else {
                // info("\tHit Req");
                dataArray->lookup(tagArray->readDataId(tagId), &req, updateReplacement);
                // respCycle += mtagLat + dataLat;
            }
        }
        gm_free(data);

        uint64_t getDoneCycle = respCycle;
        respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);

        if (evRec->hasRecord()) accessRecord = evRec->popRecord();

        // At this point we have all the info we need to hammer out the timing record
        TimingRecord tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr}; //note the end event is the response, not the wback

        if (Hit) {
            assert(!accessRecord.isValid());
            uint64_t hitLat = respCycle - req.cycle; // accLat + invLat
            HitEvent* ev = new (evRec) HitEvent(this, hitLat, domain);
            ev->setMinStartCycle(req.cycle);
            tr.startEvent = tr.endEvent = ev;
        } else {
            assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);

            // Miss events:
            // MissStart (does high-prio lookup) -> getEvent || evictionEvent || replEvent (if needed) -> MissWriteback

            MissStartEvent* mse = new (evRec) MissStartEvent(this, accLat, domain);
            MissResponseEvent* mre = new (evRec) MissResponseEvent(this, mse, domain);
            MissWritebackEvent* mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);

            mse->setMinStartCycle(req.cycle);
            mre->setMinStartCycle(getDoneCycle);
            mwe->setMinStartCycle(MAX(evDoneCycle, getDoneCycle));

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

            // Get path
            connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, getDoneCycle);
            mre->addChild(mwe, evRec);

            // Eviction path
            if (evDoneCycle) {
                connect(writebackRecord.isValid()? &writebackRecord : nullptr, mse, mwe, req.cycle + accLat, evDoneCycle);
            }

            // Replacement path
            if (evDoneCycle && cands > ways) {
                uint32_t replLookups = (cands + (ways-1))/ways - 1; // e.g., with 4 ways, 5-8 -> 1, 9-12 -> 2, etc.
                assert(replLookups);

                uint32_t fringeAccs = ways - 1;
                uint32_t accsSoFar = 0;

                TimingEvent* p = mse;

                // Candidate lookup events
                while (accsSoFar < replLookups) {
                    uint32_t preDelay = accsSoFar? 0 : tagLat;
                    uint32_t postDelay = tagLat - MIN(tagLat - 1, fringeAccs);
                    uint32_t accs = MIN(fringeAccs, replLookups - accsSoFar);
                    //// info("ReplAccessEvent rl %d fa %d preD %d postD %d accs %d", replLookups, fringeAccs, preDelay, postDelay, accs);
                    ReplAccessEvent* raEv = new (evRec) ReplAccessEvent(this, accs, preDelay, postDelay, domain);
                    raEv->setMinStartCycle(req.cycle /*lax...*/);
                    accsSoFar += accs;
                    p->addChild(raEv, evRec);
                    p = raEv;
                    fringeAccs *= ways - 1;
                }

                // Swap events -- typically, one read and one write work for 1-2 swaps. Exact number depends on layout.
                ReplAccessEvent* rdEv = new (evRec) ReplAccessEvent(this, 1, tagLat, tagLat, domain);
                rdEv->setMinStartCycle(req.cycle /*lax...*/);
                ReplAccessEvent* wrEv = new (evRec) ReplAccessEvent(this, 1, 0, 0, domain);
                wrEv->setMinStartCycle(req.cycle /*lax...*/);

                p->addChild(rdEv, evRec)->addChild(wrEv, evRec)->addChild(mwe, evRec);
            }


            tr.startEvent = mse;
            tr.endEvent = mre; // note the end event is the response, not the wback
        }
        evRec->pushRecord(tr);

        // tagArray->print();
        // dataArray->print();
    }

    cc->endAccess(req);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}
