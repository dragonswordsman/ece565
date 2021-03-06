/*
 * Copyright (c) 2003-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Erik Hallnor
 */

/**
 * @file
 * Definitions of LRUVictim tag store.
 */

#include <string>
#include <iomanip>

#include "base/intmath.hh"
#include "debug/CacheRepl.hh"
#include "mem/cache/tags/cacheset.hh"
#include "mem/cache/tags/lruVictim.hh"
#include "mem/cache/base.hh"
#include "sim/core.hh"

using namespace std;

// create and initialize a LRUVictim/MRU cache structure
LRUVictim::LRUVictim(unsigned _numSets, unsigned _blkSize, unsigned _assoc,
         unsigned _hit_latency, unsigned _victimSize)
    : numSets(_numSets), blkSize(_blkSize), assoc(_assoc),
      hitLatency(_hit_latency), victimSize(_victimSize)
{
    cout << "Associativity : "<< assoc << "victimSIZE : " << victimSize << endl;
    // Check parameters
    if (blkSize < 4 || !isPowerOf2(blkSize)) {
        fatal("Block size must be at least 4 and a power of 2");
    }
    if (numSets <= 0 || !isPowerOf2(numSets)) {
        fatal("# of sets must be non-zero and a power of 2");
    }
    if (assoc <= 0) {
        fatal("associativity must be greater than zero");
    }
    if (hitLatency <= 0) {
        fatal("access latency must be greater than zero");
    }

    blkMask = blkSize - 1;
    setShift = floorLog2(blkSize);
    setMask = numSets - 1;
    tagShift = setShift + floorLog2(numSets);
    warmedUp = false;
    /** @todo Make warmup percentage a parameter. */
    warmupBound = numSets * assoc;

    sets = new CacheSet[numSets];
    blks = new BlkType[(numSets * assoc)+victimSize];
    // allocate data storage in one big chunk
    numBlocks = (numSets * assoc)+victimSize;
    dataBlks = new uint8_t[(numBlocks+victimSize) * blkSize];

    unsigned blkIndex = 0;       // index into blks array
    for (unsigned i = 0; i < numSets; ++i) {
        sets[i].assoc = assoc;

        sets[i].blks = new BlkType*[assoc];

        // link in the data blocks
        for (unsigned j = 0; j < assoc; ++j) {
            // locate next cache block
            BlkType *blk = &blks[blkIndex];
            blk->data = &dataBlks[blkSize*blkIndex];
            ++blkIndex;

            // invalidate new cache block
            blk->invalidate();

            //EGH Fix Me : do we need to initialize blk?

            // Setting the tag to j is just to prevent long chains in the hash
            // table; won't matter because the block is invalid
            blk->tag = j;
            blk->whenReady = 0;
            blk->isTouched = false;
            blk->size = blkSize;
            sets[i].blks[j]=blk;
            blk->set = i;
        }
    }

    //  Check if victimSize > 0
    if (victimSize != 0)
    {
        DPRINTF(CacheRepl, "Building VictimCache with %d block\n", victimSize);
        victimCacheBlks = new BlkType[victimSize];
        victimLatency = 1;

        victimDataBlks = new uint8_t[victimSize * blkSize];

        victimCache = new CacheSet;
        victimCache->assoc = victimSize;
        victimCache->blks = new BlkType*[victimSize];

        for (unsigned j = 0; j < victimSize; j++)
        {
            BlkType *blk = &victimCacheBlks[j];
            blk->data = &victimDataBlks[blkSize*j];
            //++blkIndex;
            blk->invalidate();

            // Setting the tag to j is just to prevent long chains in the hash
            // table; won't matter because the block is invalid
            blk->tag = j;
            blk->whenReady = 0;
            blk->isTouched = false;
            blk->size = blkSize;
            victimCache->blks[j]=blk;
            blk->set = 0;
        }
    }
}

LRUVictim::~LRUVictim()
{
    delete [] victimDataBlks;
    delete [] victimCache;
    delete [] dataBlks;
    delete [] blks;
    delete [] sets;
}

LRUVictim::BlkType*
LRUVictim::accessBlock(Addr addr, int &lat, int master_id)
{
   Addr tag = extractTag(addr);
    unsigned set = extractSet(addr);
    BlkType *blk = sets[set].findBlk(tag);
    lat = hitLatency;
  
  if((blk == NULL) && (victimSize != 0)){
	for (int i = 0; i < victimSize; ++i) {
		if ((victimCache->blks[i]->tag == tag) && 
        (victimCache->blks[i]->set == set) 
        && (((victimCache->blks[i]->status) & 0x01) !=0)){
		
            blk = victimCache->blks[i];
            victimCache->blks[i] = sets[set].blks[assoc-1];
            victimCache->moveToHead(victimCache->blks[i]);
            sets[set].blks[assoc-1] = blk;
            lat+=victimLatency;
            break;
		}
		
	}
  }





    if (blk != NULL) {
        // move this block to head of the MRU list
        sets[set].moveToHead(blk);
        DPRINTF(CacheRepl, "set %x: moving blk %x to MRU\n",
                set, regenerateBlkAddr(tag, set));
        if (blk->whenReady > curTick()
            && blk->whenReady - curTick() > hitLatency) {
            lat = blk->whenReady - curTick();
        }
        blk->refCount += 1;
    }
    return blk;
}


LRUVictim::BlkType*
LRUVictim::findBlock(Addr addr) const
{
    ////cout << "VictimCache : findBlock Addr" << addr << endl;
    Addr tag = extractTag(addr);
    unsigned set = extractSet(addr);
    BlkType *blk = sets[set].findBlk(tag);
    return blk;
}

LRUVictim::BlkType*
LRUVictim::findVictim(Addr addr, PacketList &writebacks)
{
    unsigned set = extractSet(addr);
    // grab a replacement candidate
    BlkType *blk = sets[set].blks[assoc-1];

    if (blk->isValid()) {
        DPRINTF(CacheRepl, "set %x: selecting blk %x for replacement\n",
                set, regenerateBlkAddr(blk->tag, set));
        if(victimSize != 0){
            BlkType *tmp = victimCache->blks[victimCache->assoc-1];
            victimCache->blks[victimCache->assoc-1] = blk;
            victimCache->blks[victimCache->assoc-1]->tag = blk->tag;
            victimCache->blks[victimCache->assoc-1]->set = set;
            victimCache->moveToHead(blk);
            sets[set].blks[assoc-1] = tmp;
            blk = tmp;
		}
    }
    return blk;
}

void
LRUVictim::insertBlock(Addr addr, BlkType *blk, int master_id)
{
       if (!blk->isTouched) {
        tagsInUse++;
        blk->isTouched = true;
        if (!warmedUp && tagsInUse.value() >= warmupBound) {
            warmedUp = true;
            warmupCycle = curTick();
        }
    }

    // If we're replacing a block that was previously valid update
    // stats for it. This can't be done in findBlock() because a
    // found block might not actually be replaced there if the
    // coherence protocol says it can't be.
    if (blk->isValid()) {
        replacements[0]++;
        totalRefs += blk->refCount;
        ++sampledRefs;
        blk->refCount = 0;

        // deal with evicted block
        assert(blk->srcMasterId < cache->system->maxMasters());
        occupancies[blk->srcMasterId]--;

        blk->invalidate();
    }

    blk->isTouched = true;
    // Set tag for new block.  Caller is responsible for setting status.
    blk->tag = extractTag(addr);
    blk->set = extractSet(addr);
    // deal with what we are bringing in
    assert(master_id < cache->system->maxMasters());
    occupancies[master_id]++;
    blk->srcMasterId = master_id;

    unsigned set = extractSet(addr);
    sets[set].moveToHead(blk);
}

void
LRUVictim::invalidate(BlkType *blk)
{
    //cout << "VictimCache : invalidate : Tag " << hex << blk->tag << " Set " << blk->set 
           // << " Addr " << regenerateBlkAddr(blk->tag, blk->set) << endl;
    assert(blk);
    assert(blk->isValid());
    tagsInUse--;
    assert(blk->srcMasterId < cache->system->maxMasters());
    occupancies[blk->srcMasterId]--;
    blk->srcMasterId = Request::invldMasterId;

    // should be evicted before valid blocks
    unsigned set = blk->set;
    sets[set].moveToTail(blk);

}

void
LRUVictim::clearLocks()
{
    for (int i = 0; i < numBlocks; i++){
        blks[i].clearLoadLocks();
    }
}

void
LRUVictim::cleanupRefs()
{
    for (unsigned i = 0; i < numSets*assoc; ++i) {
        if (blks[i].isValid()) {
            totalRefs += blks[i].refCount;
            ++sampledRefs;
        }
    }
}

void inline LRUVictim::copyBlocktoVictimCache( unsigned set, unsigned blockIndex, unsigned victimCacheIndex )
{
    victimCache->blks[victimCacheIndex]->tag        = sets[set].blks[blockIndex]->tag;
    victimCache->blks[victimCacheIndex]->data       = sets[set].blks[blockIndex]->data;
    victimCache->blks[victimCacheIndex]->size       = sets[set].blks[blockIndex]->size;
    victimCache->blks[victimCacheIndex]->status     = sets[set].blks[blockIndex]->status;
    victimCache->blks[victimCacheIndex]->whenReady  = sets[set].blks[blockIndex]->whenReady;
    victimCache->blks[victimCacheIndex]->set        = sets[set].blks[blockIndex]->set;
    victimCache->blks[victimCacheIndex]->refCount   = sets[set].blks[blockIndex]->refCount;
}

void inline LRUVictim::copyBlockfromVictimCache( unsigned set, unsigned blockIndex, unsigned victimCacheIndex )
{
    sets[set].blks[blockIndex]->tag        = victimCache->blks[victimCacheIndex]->tag;
    sets[set].blks[blockIndex]->data       = victimCache->blks[victimCacheIndex]->data;
    sets[set].blks[blockIndex]->size       = victimCache->blks[victimCacheIndex]->size;
    sets[set].blks[blockIndex]->status     = victimCache->blks[victimCacheIndex]->status;
    sets[set].blks[blockIndex]->whenReady  = victimCache->blks[victimCacheIndex]->whenReady;
    sets[set].blks[blockIndex]->set        = victimCache->blks[victimCacheIndex]->set;
    sets[set].blks[blockIndex]->refCount   = victimCache->blks[victimCacheIndex]->refCount;
}

void LRUVictim::printVictimCache()
{
    cout << "Victim Cache : " << victimCache->assoc << " Blocks : " << endl;
    for (size_t i = 0; i < victimCache->assoc; i++)
    {
       BlkType *blk = victimCache->blks[i];
        cout << "  Tag : " << hex << setw(4) << setfill('0') << blk->tag << "  ";
        cout << "  Set : " << hex << setw(4) << setfill('0') << blk->set << "  ";
        cout << "  Addr : " << hex << setw(4) << setfill('0') << regenerateBlkAddr(blk->tag, blk->set) << "  ";
        cout << "  Data : " << setw(3) << setfill('0') << blk->data[0] << "  ";
        cout << "  " << setw(2) << setfill('0') << blk->data[1] << "  ";
        cout << "  " << setw(2) << setfill('0') << blk->data[2] << "  ";
        cout << "  " << setw(2) << setfill('0') << blk->data[3] << "  ";
        cout << "  " << blk->isValid() << "  ";
        cout << endl;
    }
    cout << endl;
    
}

void LRUVictim::printSet( unsigned setIndex )
{
    cout << "Set " << setIndex << " : " << endl;
    for (size_t i = 0; i < assoc; i++)
    {
        BlkType *blk = sets[setIndex].blks[i];
        cout << "  Tag : " << hex << setw(4) << setfill('0') << blk->tag << "  ";
        cout << "  Set : " << hex << setw(4) << setfill('0') << blk->set << "  ";
        cout << "  Addr : " << hex << setw(4) << setfill('0') << regenerateBlkAddr(blk->tag, blk->set) << "  ";
        cout << "  Data : " << setw(3) << setfill('0') << blk->data[0] << "  ";
        cout << "  " << setw(2) << setfill('0') << blk->data[1] << "  ";
        cout << "  " << setw(2) << setfill('0') << blk->data[2] << "  ";
        cout << "  " << setw(2) << setfill('0') << blk->data[3] << "  ";
        cout << "  " << blk->isValid() << "  ";
        cout << endl;
    }
    cout << endl;
    
}
