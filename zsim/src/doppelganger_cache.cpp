#include "doppelganger_cache.h"
#include "event_recorder.h"
#include "timing_event.h"
#include "pin.H"

DoppelgangerCache::DoppelgangerCache(uint32_t _numLines, CC* _cc, CC* _dcc, DoppelgangerTagArray* _tagArray, DoppelgangerDataArray* _dataArray, 
CacheArray* _array, ReplPolicy* tagRP, ReplPolicy* dataRP, ReplPolicy* RP, uint32_t _accLat, uint32_t _invLat, 
uint32_t mshrs, uint32_t tagLat, uint32_t mtagLat, uint32_t mapLat, uint32_t dataLat, uint32_t ways, uint32_t cands,
uint32_t _domain, const g_string& _name) : TimingCache(_numLines, _cc, _array, RP, _accLat, _invLat, mshrs, tagLat, 
ways, cands, _domain, _name), dcc(_dcc), tagArray(_tagArray), dataArray(_dataArray), tagLat(tagLat), mtagLat(mtagLat), 
mapLat(mapLat), dataLat(dataLat), tagRP(tagRP), dataRP(dataRP) {
    info("Doppelganger Created");
}

void DoppelgangerCache::setParents(uint32_t childId, const g_vector<MemObject*>& parents, Network* network) {
    dcc->setParents(childId, parents, network);
    TimingCache::setParents(childId, parents, network);
}

void DoppelgangerCache::setChildren(const g_vector<BaseCache*>& children, Network* network) {
    dcc->setChildren(children, network);
    TimingCache::setChildren(children, network);
}

uint64_t DoppelgangerCache::access(MemReq& req) {
    // info("%lu: REQ %s to address %lu.", req.cycle, AccessTypeName(req.type), req.lineAddr);
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
        // info("%lu: REQ %s to address %lu confirmed exact.", req.cycle, AccessTypeName(req.type), req.lineAddr << lineBits);
        return TimingCache::access(req);
    }
    PIN_SafeCopy(data, (void*)req.lineAddr, zinfo->lineSize);

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
    bool skipAccess = dcc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        info("%lu: REQ %s to address %lu confirmed approximate.", req.cycle, AccessTypeName(req.type), req.lineAddr << lineBits);
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t tagId = tagArray->lookup(req.lineAddr, &req, updateReplacement);
        respCycle += accLat;

        if (tagId == -1) {
            // Miss, no tags found.
            Hit = false;
            assert(dcc->shouldAllocate(req));

            // Map calculation for Doppelganger is also off the critical path.
            uint32_t map = dataArray->calculateMap(data, type, min, max);
            // info("\tMiss Req data type: %s, data: %s", DataTypeName(type), (char*)data);
            info("\tMiss Req map: %u", map);
            int32_t mapId = dataArray->lookup(map, &req, updateReplacement);
            if (tag != -1) {
                info("\tSimilar map at: %i", tag);
                // Found similar mtag, allocate new tag and evict another if necessary.
                Address wbLineAddr;
                lineId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

                info("\t\tEvicting tag: %i", lineId);
                evDoneCycle = dcc->processEviction(req, wbLineAddr, lineId, respCycle); //if needed, send invalidates/downgrades to lower level, and wb to upper level
                bool clearOld;
                int32_t oldHead;
                int32_t mapID;
                tagArray->postinsert(req.lineAddr, &req, lineId, map, tag, &clearOld, &oldHead, &mapID);
                if (clearOld) {
                    dataArray->clear(mapID);
                } else if (oldHead != -1) {
                    dataArray->changeTag(mapID, oldHead, &req, updateReplacement);
                }
                dataArray->changeTag(map, lineId, &req, updateReplacement);
                
                if (evRec->hasRecord()) {
                    writebackRecord.clear();
                    writebackRecord = evRec->popRecord();
                    writebackRecords.push_back(writebackRecord);
                }
            } else {
                info("\tNo similar map");
                // couldn't find a similar mtag, allocate new data/mtag and evict another
                // if necessary, evict the tags associated with it too.
                int32_t listHead;
                uint32_t mapLineId = dataArray->preinsert(map, &req, &listHead);
                evDoneCycle = respCycle;
                while (listHead != -1) {
                    Address wbLineAddr = tagArray->read(listHead);
                    info("\t\tEvicting tag: %i", listHead);
                    evDoneCycle = dcc->processEviction(req, wbLineAddr, listHead, evDoneCycle); //if needed, send invalidates/downgrades to lower level, and wb to upper level
                    listHead = tagArray->walkLinkedList(listHead);
                    if (evRec->hasRecord()) {
                        writebackRecord.clear();
                        writebackRecord = evRec->popRecord();
                        writebackRecords.push_back(writebackRecord);
                    }
                }

                //Make space for new line
                Address wbLineAddr;
                lineId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

                //Evictions are not in the critical path in any sane implementation -- we do not include their delays
                //NOTE: We might be "evicting" an invalid line for all we know. Coherence controllers will know what to do
                info("\t\tEvicting tag: %i", lineId);
                evDoneCycle = dcc->processEviction(req, wbLineAddr, lineId, evDoneCycle); //if needed, send invalidates/downgrades to lower level, and wb to upper level
                if (evRec->hasRecord()) {
                    writebackRecord.clear();
                    writebackRecord = evRec->popRecord();
                    writebackRecords.push_back(writebackRecord);
                }
                bool clearOld;
                int32_t oldHead;
                int32_t mapID;
                tagArray->postinsert(req.lineAddr, &req, lineId, map, -1, &clearOld, &oldHead, &mapID);
                if (clearOld) {
                    dataArray->clear(mapID);
                } else if (oldHead != -1) {
                    dataArray->changeTag(mapID, oldHead, &req, updateReplacement);
                }
                dataArray->postinsert(map, &req, mapLineId, lineId);
            }
        } else {
            // This is a hit
            Hit = true;
            if (req.type == PUTX) {
                // If this is a write
                uint32_t map = dataArray->calculateMap(data, type, min, max);
                // info("\tHit Req data type: %s, data: %s", DataTypeName(type), (char*)data);
                info("\tHit PUTX Req map: %u", map);
                // respCycle += mapLat;

                if ((int32_t)map == lineId) {
                    info("\tMap not changed");
                    // and its map is the same as before. Do nothing.
                    dataArray->lookup(lineId, &req, updateReplacement);
                    // respCycle += mtagLat + dataLat;
                } else {
                    // and its map is not the same as before.
                    int32_t tag = dataArray->findSimilarLine(map);
                    if (tag != -1) {
                        info("\tSimilar map at: %i", tag);
                        // but is similar to something else that exists.
                        // we only need to add the tag to the existing linked
                        // list.
                        bool clearOld;
                        int32_t oldHead;
                        tagArray->changeLinkedList(req.lineAddr, tag, 0, &clearOld, &oldHead);
                        dataArray->changeTag(map, tag, &req, updateReplacement);
                        if (clearOld) {
                            dataArray->clear(lineId);
                        } else if (oldHead != -1) {
                            dataArray->changeTag(lineId, tag, &req, updateReplacement);
                        }
                        // respCycle += MAX(mtagLat, dataLat);
                    } else {
                        info("\tNo similar map");
                        // and is also not similar to anything we have, we 
                        // need to allocate new data, and evict another if we
                        // have to.
                        int32_t listHead;
                        uint32_t mapLineId = dataArray->preinsert(map, &req, &listHead);
                        evDoneCycle = respCycle;
                        while (listHead != -1) {
                            Address wbLineAddr = tagArray->read(listHead);
                            info("\t\tEvicting tag: %i", listHead);
                            evDoneCycle = dcc->processEviction(req, wbLineAddr, listHead, evDoneCycle); //if needed, send invalidates/downgrades to lower level, and wb to upper level
                            listHead = tagArray->walkLinkedList(listHead);
                            if (evRec->hasRecord()) {
                                writebackRecord.clear();
                                writebackRecord = evRec->popRecord();
                                writebackRecords.push_back(writebackRecord);
                            }
                        }
                        bool clearOld;
                        int32_t oldHead;
                        int32_t id = tagArray->changeLinkedList(req.lineAddr, -1, map, &clearOld, &oldHead);
                        if (clearOld) {
                            dataArray->clear(lineId);
                        } else if (oldHead != -1) {
                            dataArray->changeTag(lineId, oldHead, &req, updateReplacement);
                        }
                        dataArray->postinsert(map, &req, mapLineId, id);
                        // respCycle += mtagLat;
                    }
                }
            } else {
                info("\tHit Req");
                dataArray->lookup(lineId, &req, updateReplacement);
                // respCycle += mtagLat + dataLat;
            }
        }
        gm_free(data);

        uint64_t getDoneCycle = respCycle;
        respCycle = dcc->processAccess(req, lineId, respCycle, &getDoneCycle);

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
                    //info("ReplAccessEvent rl %d fa %d preD %d postD %d accs %d", replLookups, fringeAccs, preDelay, postDelay, accs);
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

        tagArray->print();
        dataArray->print();
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
        info("%lu: INV to approximate address %lu.", req.cycle, req.lineAddr << lineBits);
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
    int32_t lineId = array->lookup(req.lineAddr, nullptr, false);
    assert_msg(lineId != -1, "[%s] Invalidate on non-existing address 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    uint64_t respCycle = req.cycle + invLat;
    trace(Cache, "[%s] Invalidate start 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    respCycle = dcc->processInv(req, lineId, respCycle); //send invalidates or downgrades to children, and adjust our own state
    trace(Cache, "[%s] Invalidate end 0x%lx type %s lineId %d, reqWriteback %d, latency %ld", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback, respCycle - req.cycle);

    return respCycle;
}