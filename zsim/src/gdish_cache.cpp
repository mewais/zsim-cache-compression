#include "gdish_cache.h"
#include "pin.H"

GDISHCache::GDISHCache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, ApproximateBDITagArray* _tagArray, ApproximateBDIDataArray* _dataArray,
ReplPolicy* tagRP, ReplPolicy* dataRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t ways, uint32_t cands, uint32_t _domain, const g_string& _name,
RunningStats* _crStats, RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, uint32_t _dedupBufferSize) : TimingCache(_numTagLines, _cc, NULL, tagRP,
_accLat, _invLat, mshrs, tagLat, ways, cands, _domain, _name), numTagLines(_numTagLines), numDataLines(_numDataLines), tagArray(_tagArray), tagRP(tagRP), crStats(_crStats),
evStats(_evStats), tutStats(_tutStats), dutStats(_dutStats), dedupBufferSize(_dedupBufferSize) {cacheTimestamp = 0;}

void GDISHCache::initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name.c_str(), "GDISH cache stats");
    initCacheStats(cacheStat);

    //Stats specific to timing cacheStat
    profOccHist.init("occHist", "Occupancy MSHR cycle histogram", numMSHRs+1);
    cacheStat->append(&profOccHist);

    profHitLat.init("latHit", "Cumulative latency accesses that hit (demand and non-demand)");
    profMissRespLat.init("latMissResp", "Cumulative latency for miss start to response");
    profMissLat.init("latMiss", "Cumulative latency for miss start to finish (free MSHR)");

    cacheStat->append(&profHitLat);
    cacheStat->append(&profMissRespLat);
    cacheStat->append(&profMissLat);

    parentStat->append(cacheStat);
}

void GDISHCache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    tagArray->initStats(cacheStat);
    tagRP->initStats(cacheStat);
}

uint64_t GDISHCache::access(MemReq& req) {
    DataLine data = gm_calloc<uint8_t>(zinfo->lineSize);
    DataType type = ZSIM_FLOAT;
    bool approximate = false;
    uint64_t Evictions = 0;
    uint64_t readAddress = req.lineAddr;
    if (zinfo->realAddresses->find(req.lineAddr) != zinfo->realAddresses->end())
        readAddress = (*zinfo->realAddresses)[req.lineAddr];
    for(uint32_t i = 0; i < zinfo->approximateRegions->size(); i++) {
        if ((readAddress << lineBits) >= std::get<0>((*zinfo->approximateRegions)[i]) && (readAddress << lineBits) <= std::get<1>((*zinfo->approximateRegions)[i])
        && (readAddress << lineBits)+zinfo->lineSize-1 >= std::get<0>((*zinfo->approximateRegions)[i]) && (readAddress << lineBits)+zinfo->lineSize-1 <= std::get<1>((*zinfo->approximateRegions)[i])) {
            type = std::get<2>((*zinfo->approximateRegions)[i]);
            approximate = true;
            break;
        }
    }
    PIN_SafeCopy(data, (void*)(readAddress << lineBits), zinfo->lineSize);

    EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
    assert_msg(evRec, "GDISH is not connected to TimingCore");

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
                // // // info("uCREATE: %p at %u", dUp, __LINE__);
                dUp->setMinStartCycle(startCycle);
                startEv->addChild(dUp, evRec)->addChild(r->startEvent, evRec);
            } else {
                startEv->addChild(r->startEvent, evRec);
            }

            if (downLat) {
                DelayEvent* dDown = new (evRec) DelayEvent(downLat);
                // // // info("uCREATE: %p at %u", dDown, __LINE__);
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
                // // // info("uCREATE: %p at %u", dEv, __LINE__);
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

    g_vector<uint32_t> keptFromEvictions;

    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        // info("%lu: REQ %s to address %lu in %s region", req.cycle, AccessTypeName(req.type), req.lineAddr << lineBits, approximate? "approximate":"exact");
        // info("Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t tagId = tagArray->lookup(req.lineAddr, &req, updateReplacement);
        respCycle += accLat;
        evictCycle += accLat;

        MissStartEvent* mse;
        MissResponseEvent* mre;
        MissWritebackEvent* mwe;
        if (tagId == -1) {
            // info("\tTag Miss");
            assert(cc->shouldAllocate(req));
            // Get the eviction candidate
            Address wbLineAddr;
            int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            // info("\t\tEvicting tagId: %i", victimTagId);
            keptFromEvictions.push_back(victimTagId);
            trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);
            // Need to evict the tag.
            tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
            // // // info("\t\t\tEviction finished at %lu", tagEvDoneCycle);
            if (evRec->hasRecord()) {
                // info("\t\tand its data of size %i segments", BDICompressionToSize(tagArray->readCompressionEncoding(victimTagId), zinfo->lineSize)/8);
                // // info("\t\tEvicting tagId: %i", victimTagId);
                Evictions++;
                tagWritebackRecord.clear();
                tagWritebackRecord = evRec->popRecord();
            }
            switch(tagArray->readCompressionType(victimTagId)) {
                case DEDUP:
                    int32_t dedupVictim = tagArray->readSegmentPointer(victimTagId);
                    assert(dedupVictim != -1);
                    if (tagArray->readTimestamp(victimTagId) >= dedupArray->readTimestamp(dedupVictim)) {
                        dedupArray->decreaseCounter(dedupVictim);
                    }
                    break;
                case SCHEME1:
                case SCHEME2:
                    g_vector<uint32_t> dicts = tagArray->readDictIndices(victimTagId);
                    g_vector<uint32_t> timestamps = dictionary->readTimestamps(dicts);
                    for (uint32_t i = 0; i < timestamps.size(); i++) {
                        if (tagArray->readTimestamp(victimTagId) >= timestamps[i]) {
                            dictionary->decreaseCounter(dicts[i]);
                        }
                    }
                    break;
                case NONE:
                    break;
                default:
                    panic("Illegal Value");
            }

            // Need to get the line we want
            uint64_t getDoneCycle = respCycle;
            respCycle = cc->processAccess(req, victimTagId, respCycle, &getDoneCycle);
            tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
            if (evRec->hasRecord()) accessRecord = evRec->popRecord();

            // Now compress (and approximate) the new line
            if (approximate)
                dataArray->approximate(data, type);

            // Check if line can be deduped
            int32_t dedupTarget = dedupArray->exists(data);
            if (dedupTarget != -1) {
                // Already in dedup array, good for us.
                g_vector<uint32_t> dummy;
                dedupArray->increaseCounter(dedupTarget);
                tagArray->postinsert(req.lineAddr, &req, victimTagId, cacheTimestamp, DEDUP, dedupTarget, dummy, approximate);

                MissStartEvent* mse = new (evRec) MissStartEvent(this, accLat, domain);
                MissResponseEvent* mre = new (evRec) MissResponseEvent(this, mse, domain);
                MissWritebackEvent* mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);

                mse->setMinStartCycle(req.cycle);
                mre->setMinStartCycle(respCycle);
                mwe->setMinStartCycle(MAX(evDoneCycle, getDoneCycle));

                // Get path
                connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, getDoneCycle);
                mre->addChild(mwe, evRec);

                // Eviction path
                if (evDoneCycle) {
                    connect(writebackRecord.isValid()? &writebackRecord : nullptr, mse, mwe, req.cycle + accLat, evDoneCycle);
                }
            } else {
                // Not in dedup array, check temporary dedup buffer
                uint32_t dedupVictimIndex = 0;
                uint32_t dedupVictimRank = 0;
                dedupVictimIndex = preinsert(&dedupVictimRank);
                // Otherwise it's not worth the search in the queue
                if (dedupVictimRank < dedupBufferSize) {
                    uint32_t promotionRank = 0;
                    g_vecor<uint32_t> promotionTags;
                    g_vecor<Address> promotionAddresses;
                    for (uint32_t i = 0; i < dedupBufferSize; i++) {
                        if (std::memcmp(dedupBuffer[i], data, zinfo->lineSize) == 0) {
                            if (tagArray->readCompressionType(dedupBufferTags[i]) == NONE) {
                                promotionRank++;
                                PromotionTags.push_back(dedupBufferTags[i]);
                                promotionAddresses.push_back(dedupBufferAddresses[i]);
                            }
                        }
                    }
                    if (promotionRank > dedupVictimRank) {
                        // Found in the queue, Insert.
                        dedupArray->postinsert(dedupVictimIndex, data, cacheTimestamp);
                        for (uint32_t i = 0; i < promotionTags; i++) {
                            respCycle += accLat;
                            g_vector<uint32_t> dummy;
                            bool prevApprox = tagArray->readApproximate(promotionTags[i]);
                            tagArray->postinsert(promotionAddresses[i], &req, promotionTags[i], cacheTimestamp, DEDUP, dedupVictimIndex, dummy, prevApprox);
                            dedupArray->increaseCounter(dedupVictimIndex);
                        }
                        tagArray->postinsert(req.lineAddr, &req, victimTagId, cacheTimestamp, DEDUP, dedupVictimIndex, dummy, approximate);
                        MissStartEvent* mse = new (evRec) MissStartEvent(this, accLat, domain);
                        MissResponseEvent* mre = new (evRec) MissResponseEvent(this, mse, domain);
                        MissWritebackEvent* mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);

                        mse->setMinStartCycle(req.cycle);
                        mre->setMinStartCycle(respCycle);
                        mwe->setMinStartCycle(MAX(evDoneCycle, getDoneCycle));

                        // Get path
                        connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, getDoneCycle);
                        mre->addChild(mwe, evRec);

                        // Eviction path
                        if (evDoneCycle) {
                            connect(writebackRecord.isValid()? &writebackRecord : nullptr, mse, mwe, req.cycle + accLat, evDoneCycle);
                        }
                    } else {
                        // Now that dedup failed, go with GDISH
                        goto NO_GDISH;    // Yeah yeah, nobody likes goto, I do!
                    }
                } else {
                    // Now that dedup failed, go with GDISH
NO_GDISH:
                    pushDedupQueue(req.lineAddr, victimTagId, data);
                    // Compress the line with both DISH schemes.
                    g_vector<uint32_t> scheme1Dict, scheme2Dict;
                    dataArray->compress(data, scheme1Dict, scheme2Dict);
                    
                    g_vector<uint32_t> scheme1Index, scheme1NotFound;
                    g_vector<uint32_t> scheme2Index, scheme2NotFound;
                    for (uint32_t i = 0; i < scheme1Dict.size(); i++) {
                        uint32_t exists1 = dictionary->exists(scheme1Dict[i]);
                        uint32_t exists2 = dictionary->exists(scheme2Dict[i]);
                        if (exists1 == -1) {
                            scheme1NotFound.push_back(scheme1Dict[i]);
                        } else {
                            scheme1Index.push_back(exists1);
                        }
                        if (exists2 == -1) {
                            scheme2NotFound.push_back(scheme2Dict[i]);
                        } else {
                            scheme2Index.push_back(exists2);
                        }
                    }
                    // If it's fully in Dictionary, we're done
                    if (scheme1NotFound.size() == 0) {
                        for (uint32_t i = 0; i < scheme1Index.size(); i++) {
                            dictionary->increaseCounter(scheme1Index[i]);
                        }
                        // Clear enough data size before actual insertion.
                        uint32_t lineSize = (zinfo->lineSize()/4)*((uint32_t)(32/log2(dictionary->getNumLines())));
                        uint64_t evBeginCycle = respCycle + 1;
                        int32_t victimTagId2 = tagArray->needEviction(req.lineAddr, &req, lineSize, keptFromEvictions, &wbLineAddr);
                        TimingRecord writebackRecord;
                        uint64_t lastEvDoneCycle = tagEvDoneCycle;
                        g_vector<uint32_t> dummy;
                        while(victimTagId2 != -1) {
                            // info("\t\tEvicting tagId: %i", victimTagId2);
                            keptFromEvictions.push_back(victimTagId2);
                            // uint32_t size = BDICompressionToSize(tagArray->readCompressionEncoding(victimTagId2), zinfo->lineSize)/8;
                            uint64_t evDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId2, evBeginCycle);
                            // // // info("\t\t\tEviction finished at %lu", evDoneCycle);
                            tagArray->postinsert(0, &req, victimTagId2, 0, NONE, -1, dummy, false);
                            if (evRec->hasRecord()) {
                                // // info("\t\tEvicting tagId: %i", victimTagId2);
                                // info("\t\tand freed %i segments", size);
                                Evictions++;
                                writebackRecord.clear();
                                writebackRecord = evRec->popRecord();
                                writebackRecords.push_back(writebackRecord);
                                wbStartCycles.push_back(evBeginCycle);
                                wbEndCycles.push_back(evDoneCycle);
                                lastEvDoneCycle = evDoneCycle;
                                evBeginCycle += 1;
                            }
                            victimTagId2 = tagArray->needEviction(req.lineAddr, &req, lineSize, keptFromEvictions, &wbLineAddr);
                        }
                        tagArray->postinsert(req.lineAddr, &req, victimTagId, cacheTimestamp, SCHEME1, 1, dummy, approximate);
                        mse = new (evRec) MissStartEvent(this, accLat, domain);
                        // // // info("uCREATE: %p at %u", mse, __LINE__);
                        mre = new (evRec) MissResponseEvent(this, mse, domain);
                        // // // info("uCREATE: %p at %u", mre, __LINE__);
                        mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);
                        // // // info("uCREATE: %p at %u", mwe, __LINE__);
                        mse->setMinStartCycle(req.cycle);
                        // // // info("\t\t\tMiss Start Event: %lu, %u", req.cycle, accLat);
                        mre->setMinStartCycle(respCycle);
                        // // // info("\t\t\tMiss Response Event: %lu", respCycle);
                        mwe->setMinStartCycle(MAX(lastEvDoneCycle, tagEvDoneCycle));
                        // // // info("\t\t\tMiss writeback event: %lu, %u", MAX(lastEvDoneCycle, tagEvDoneCycle), accLat);
                        connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, respCycle);
                        if(wbStartCycles.size()) {
                            for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                                DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - respCycle);
                                // // // info("uCREATE: %p at %u", del, __LINE__);
                                del->setMinStartCycle(respCycle);
                                mre->addChild(del, evRec);
                                connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, mwe, wbStartCycles[i], wbEndCycles[i]);
                            }
                        }
                        mre->addChild(mwe, evRec);
                        if (tagEvDoneCycle) {
                            connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, mse, mwe, req.cycle + accLat, tagEvDoneCycle);
                        }
                    } else if (scheme2NotFound.size() == 0) {
                        for (uint32_t i = 0; i < scheme2Index.size(); i++) {
                            dictionary->increaseCounter(scheme2Index[i]);
                        }
                        // Clear enough data size before actual insertion.
                        uint32_t lineSize = (zinfo->lineSize()/4)*((uint32_t)(32/log2(dictionary->getNumLines())));
                        uint64_t evBeginCycle = respCycle + 1;
                        int32_t victimTagId2 = tagArray->needEviction(req.lineAddr, &req, lineSize, keptFromEvictions, &wbLineAddr);
                        TimingRecord writebackRecord;
                        uint64_t lastEvDoneCycle = tagEvDoneCycle;
                        g_vector<uint32_t> dummy;
                        while(victimTagId2 != -1) {
                            // info("\t\tEvicting tagId: %i", victimTagId2);
                            keptFromEvictions.push_back(victimTagId2);
                            // uint32_t size = BDICompressionToSize(tagArray->readCompressionEncoding(victimTagId2), zinfo->lineSize)/8;
                            uint64_t evDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId2, evBeginCycle);
                            // // // info("\t\t\tEviction finished at %lu", evDoneCycle);
                            tagArray->postinsert(0, &req, victimTagId2, 0, NONE, -1, dummy, false);
                            if (evRec->hasRecord()) {
                                // // info("\t\tEvicting tagId: %i", victimTagId2);
                                // info("\t\tand freed %i segments", size);
                                Evictions++;
                                writebackRecord.clear();
                                writebackRecord = evRec->popRecord();
                                writebackRecords.push_back(writebackRecord);
                                wbStartCycles.push_back(evBeginCycle);
                                wbEndCycles.push_back(evDoneCycle);
                                lastEvDoneCycle = evDoneCycle;
                                evBeginCycle += 1;
                            }
                            victimTagId2 = tagArray->needEviction(req.lineAddr, &req, lineSize, keptFromEvictions, &wbLineAddr);
                        }
                        tagArray->postinsert(req.lineAddr, &req, victimTagId, cacheTimestamp, SCHEME2, 1, dummy, approximate);
                        mse = new (evRec) MissStartEvent(this, accLat, domain);
                        // // // info("uCREATE: %p at %u", mse, __LINE__);
                        mre = new (evRec) MissResponseEvent(this, mse, domain);
                        // // // info("uCREATE: %p at %u", mre, __LINE__);
                        mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);
                        // // // info("uCREATE: %p at %u", mwe, __LINE__);
                        mse->setMinStartCycle(req.cycle);
                        // // // info("\t\t\tMiss Start Event: %lu, %u", req.cycle, accLat);
                        mre->setMinStartCycle(respCycle);
                        // // // info("\t\t\tMiss Response Event: %lu", respCycle);
                        mwe->setMinStartCycle(MAX(lastEvDoneCycle, tagEvDoneCycle));
                        // // // info("\t\t\tMiss writeback event: %lu, %u", MAX(lastEvDoneCycle, tagEvDoneCycle), accLat);
                        connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, respCycle);
                        if(wbStartCycles.size()) {
                            for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                                DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - respCycle);
                                // // // info("uCREATE: %p at %u", del, __LINE__);
                                del->setMinStartCycle(respCycle);
                                mre->addChild(del, evRec);
                                connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, mwe, wbStartCycles[i], wbEndCycles[i]);
                            }
                        }
                        mre->addChild(mwe, evRec);
                        if (tagEvDoneCycle) {
                            connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, mse, mwe, req.cycle + accLat, tagEvDoneCycle);
                        }
                    } else if (scheme1Index.size() == 0 && scheme2Index.size() == 0) {
                        std::vector<uint32_t> Index;
                        Index.push_back(victimTagId);
                        for (uint32_t i = 0; i < scheme1NotFound.size(); i++) {
                            pushDishQueue(scheme1NotFound[i], Index);
                        }
                        tagArray->postinsert(req.lineAddr, &req, victimTagId, cacheTimestamp, NONE, 1, scheme1NotFound, approximate);
                    } else {
                        if (scheme1Index.size() >= scheme2Index.size()) {
                            uint32_t NumLines = 0;
                            std::map<uint32_t, uint32_t> lines;
                            for (uint32_t i = 0; i < scheme1NotFound.size(); size++) {
                                for (uint32_t j = 0; j < dishBuffer.size(); j++) {
                                    if (dishBuffer[j] == scheme1NotFound[i]) {
                                        for (uint32_t k = 0; k < dishBufferLineLists[j].size(); k++) {
                                            lines[dishBufferLineLists[j][k]]++;
                                        }
                                        break;
                                    }
                                }
                            }
                            for (auto i = lines.begin(); i != lines.end(); i++) {
                                if (i->second == tagArray->readNumDicts(i->first, SCHEME1)) {
                                    NumLines++;
                                }
                            }
                            g_vector<uint32_t> DictVictims = dictionary->preinsert(scheme1NotFound.size(), NumLines);

                        }
                    }
                }
            }
        } else {
        }
        cacheTimestamp++;
        gm_free(data);
        evRec->pushRecord(tr);
    }
    cc->endAccess(req);

    // info("Valid Tags: %u", tagArray->getValidLines());
    // info("Valid Lines: %u", tagArray->getDataValidSegments()/8);
    // assert(tagArray->getValidLines() == tagArray->countValidLines());
    // assert(tagArray->getDataValidSegments() == tagArray->countDataValidSegments());
    assert(tagArray->getValidLines() <= numTagLines);
    assert(tagArray->getDataValidSegments() <= numDataLines*8);
    assert(tagArray->getValidLines() >= tagArray->getDataValidSegments()/8);
    double sample = ((double)tagArray->getDataValidSegments()/8)/(double)tagArray->getValidLines();
    crStats->add(sample,1);

    if (req.type != PUTS) {
        sample = Evictions;
        evStats->add(sample,1);
    }

    sample = ((double)tagArray->getDataValidSegments()/8)/numDataLines;
    dutStats->add(sample, 1);

    sample = (double)tagArray->getValidLines()/numTagLines;
    tutStats->add(sample, 1);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void GDISHCache::simulateHitWriteback(aHitWritebackEvent* ev, uint64_t cycle, HitEvent* he) {
    uint64_t lookupCycle = tryLowPrioAccess(cycle);
    if (lookupCycle) { //success, release MSHR
        if (!pendingQueue.empty()) {
            //// // info("XXX %ld elems in pending queue", pendingQueue.size());
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

void GDISHCache::pushDedupQueue(Address address, uint32_t tag, DataLine data) {
    DataLine pushData = gm_calloc<uint8_t>(zinfo->lineSize);
    PIN_SafeCopy(pushData, data, zinfo->lineSize);
    dedupBuffer.push_front(pushData);
    dedupBufferTags.push_front(tag);
    dedupBufferAddresses.push_front(address);
    if (dedupBuffer.size() > dedupBufferSize) {
        pushData = dedupBuffer.back();
        gm_free(pushData);
        dedupBuffer.pop_back();
        dedupBufferTags.pop_back();
        dedupBufferAddresses.pop_back();
    }
}

void GDISHCache::pushDishQueue(uint32_t dish, std::vector<uint32_t> lines) {
    dishBuffer.push_front(dish);
    dishBufferLineLists.push_front(lines);
    if (dishBuffer.size() > dishBufferSize) {
        dishBuffer.pop_back();
        dishBufferLineLists.pop_back();
    }
}
