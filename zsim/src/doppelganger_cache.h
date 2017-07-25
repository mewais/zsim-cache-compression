#ifndef DOPPELGANGER_CACHE_H_
#define DOPPELGANGER_CACHE_H_

#include "timing_cache.h"

class HitWritebackEvent;

class DoppelgangerCache : public TimingCache {
    protected:
        // Cache stuff
        CC* dcc;
        DoppelgangerTagArray* tagArray;
        DoppelgangerDataArray* dataArray;

        uint32_t tagLat;
        uint32_t mtagLat;
        uint32_t mapLat;
        uint32_t dataLat;

        ReplPolicy* tagRP;
        ReplPolicy* dataRP;

    public:
        DoppelgangerCache(uint32_t _numLines, CC* _cc, CC* _dcc, DoppelgangerTagArray* _tagArray, DoppelgangerDataArray* _dataArray, 
                        CacheArray* _array, ReplPolicy* tagRP, ReplPolicy* dataRP, ReplPolicy* RP, uint32_t _accLat, uint32_t _invLat, 
                        uint32_t mshrs, uint32_t tagLat, uint32_t mtagLat, uint32_t mapLat, uint32_t dataLat, uint32_t ways, 
                        uint32_t cands, uint32_t _domain, const g_string& _name);

        uint64_t access(MemReq& req);
        void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network);
        void setChildren(const g_vector<BaseCache*>& children, Network* network);

        //NOTE: reqWriteback is pulled up to true, but not pulled down to false.
        uint64_t invalidate(const InvReq& req);
        
        void simulateHitWriteback(HitWritebackEvent* ev, uint64_t cycle, HitEvent* he);
        
    protected:
        void initCacheStats(AggregateStat* cacheStat);
        void startInvalidate(const InvReq& req); // grabs cc's downLock
        uint64_t finishInvalidate(const InvReq& req); // performs inv and releases downLock
};

class HitWritebackEvent : public TimingEvent {
    private:
        DoppelgangerCache* cache;
        HitEvent* he;
    public:
        HitWritebackEvent(DoppelgangerCache* _cache,  HitEvent* _he, uint32_t postDelay, int32_t domain) : TimingEvent(0, postDelay, domain), cache(_cache), he(_he) {}
        void simulate(uint64_t startCycle) {cache->simulateHitWriteback(this, startCycle, he);}
};

#endif // DOPPELGANGER_CACHE_H_