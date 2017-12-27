#ifndef GDISH_CACHE_H_
#define GDISH_CACHE_H_

#include "timing_cache.h"
#include "stats.h"

class gdHitWritebackEvent;

class ApproximateBDICache : public TimingCache {
    protected:
        // Cache stuff
        uint32_t numTagLines;
        uint32_t numDataLines;

        GDishTagArray* tagArray;
        GDishDataArray* dataArray;
        GDishDedupArray* dedupArray;
        GDishDictionary* dictionary;

        ReplPolicy* tagRP;
        ReplPolicy* dataRP;

        RunningStats* crStats;
        RunningStats* evStats;
        RunningStats* tutStats;
        RunningStats* dutStats;

        uint64_t cacheTimestamp;
        std::deque<DataLine> dedupBuffer;
        std::deque<uint32_t> dedupBufferTags;
        std::deque<Address> dedupBufferAddresses;
        uint32_t dedupBufferSize;
        std::deque<uint32_t> dishBuffer;
        std::deque<std::vector<uint32_t>> dishBufferLineLists;
        uint32_t gdishBufferSize;

    public:
        ApproximateBDICache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, ApproximateBDITagArray* _tagArray, ApproximateBDIDataArray* _dataArray, ReplPolicy* tagRP, ReplPolicy* dataRP, uint32_t _accLat, uint32_t _invLat,
                        uint32_t mshrs, uint32_t ways, uint32_t cands, uint32_t _domain, const g_string& _name, RunningStats* _crStats, RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats);

        uint64_t access(MemReq& req);

        void initStats(AggregateStat* parentStat);
        void simulateHitWriteback(gdHitWritebackEvent* ev, uint64_t cycle, HitEvent* he);

    protected:
        void initCacheStats(AggregateStat* cacheStat);
        void pushDedupQueue(Address address, uint32_t tag, DataLine data);
};

class gdHitWritebackEvent : public TimingEvent {
    private:
        ApproximateBDICache* cache;
        HitEvent* he;
    public:
        gdHitWritebackEvent(ApproximateBDICache* _cache,  HitEvent* _he, uint32_t postDelay, int32_t domain) : TimingEvent(0, postDelay, domain), cache(_cache), he(_he) {}
        void simulate(uint64_t startCycle) {cache->simulateHitWriteback(this, startCycle, he);}
};

#endif // GDISH_CACHE_H_
