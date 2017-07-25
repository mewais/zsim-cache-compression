#ifndef UNIDOPPELGANGER_CACHE_H_
#define UNIDOPPELGANGER_CACHE_H_

#include "timing_cache.h"

class uHitWritebackEvent;

class uniDoppelgangerCache : public TimingCache {
    protected:
        // Cache stuff
        uniDoppelgangerTagArray* tagArray;
        uniDoppelgangerDataArray* dataArray;

        uint32_t tagLat;
        uint32_t mtagLat;
        uint32_t mapLat;
        uint32_t dataLat;

        ReplPolicy* tagRP;
        ReplPolicy* dataRP;

    public:
        uniDoppelgangerCache(uint32_t _numLines, CC* _cc, uniDoppelgangerTagArray* _tagArray, uniDoppelgangerDataArray* _dataArray, CacheArray* _array,
                        ReplPolicy* tagRP, ReplPolicy* dataRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t tagLat, 
                        uint32_t mtagLat, uint32_t mapLat, uint32_t dataLat, uint32_t ways, uint32_t cands, uint32_t _domain, 
                        const g_string& _name);

        uint64_t access(MemReq& req);

        void simulateHitWriteback(uHitWritebackEvent* ev, uint64_t cycle, HitEvent* he);

    protected:
        void initCacheStats(AggregateStat* cacheStat);
};

class uHitWritebackEvent : public TimingEvent {
    private:
        uniDoppelgangerCache* cache;
        HitEvent* he;
    public:
        uHitWritebackEvent(uniDoppelgangerCache* _cache,  HitEvent* _he, uint32_t postDelay, int32_t domain) : TimingEvent(0, postDelay, domain), cache(_cache), he(_he) {}
        void simulate(uint64_t startCycle) {cache->simulateHitWriteback(this, startCycle, he);}
};

#endif // UNIDOPPELGANGER_CACHE_H_