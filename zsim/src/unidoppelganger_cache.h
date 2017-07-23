#ifndef UNIDOPPELGANGER_CACHE_H_
#define UNIDOPPELGANGER_CACHE_H_

#include "timing_cache.h"

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

    protected:
        void initCacheStats(AggregateStat* cacheStat);
};

#endif // UNIDOPPELGANGER_CACHE_H_