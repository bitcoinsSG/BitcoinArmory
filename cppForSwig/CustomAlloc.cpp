#include "CustomAlloc.h"

namespace CustomAlloc
{  
   size_t getPageSize()
   {
      #ifdef _MSC_VER
         SYSTEM_INFO si;
	      GetSystemInfo(&si);   

         return si.dwPageSize;
      #else
         return sysconf(_SC_PAGE_SIZE);
      #endif
   }

   size_t getPoolSize()
   {
      //align the mempool size to the multiple of the pagesize closest to target
      size_t pageSize = getPageSize();
      size_t target = 512*1024;


      if(pageSize < target)
      {
         int mult = target / pageSize +1;
         pageSize *= mult;
      }

      return pageSize;
   }

   size_t memMinHardTop = 1024*1024*100;
   size_t memMaxHardTop = 1024*1024*1024;

   #ifdef _MSC_VER
      HANDLE processHandle = GetCurrentProcess();
   #endif

   int extendLockedMemQuota(int64_t quota)
   {
      /***
      Sets the quota for locked memory.
      This function is meant to allow the process to dynamically grow its
      mlockable memory base. A hard ceiling is set at 100mb.
      ***/

      int rt = 0;

   #ifdef _MSC_VER
      /***
      MSVS only approach: WinAPI's SetProcessWorkingSetSize:
      A few rules to follow with this API call:
      1) Windows requires 20 memory pages of overhead
      2) Your lockable memory is defined only by the minimum value
      3) The maximum value defines how much maximum RAM Windows will try to 
      reserve for this process in case of system wide memory shortage.
      Essentially this is just a guideline as memory is delivered on a first
      come first served basis. Set to 1GB for good measure.
      ***/
      size_t pageSize = getPageSize();

      //all values have to be a multiple of the system pagesize

      size_t mincurrent, maxcurrent;

      GetProcessWorkingSetSize(processHandle, (PSIZE_T)&mincurrent, (PSIZE_T)&maxcurrent);

      mincurrent /= pageSize;
      if(mincurrent<20) mincurrent = 20;

      quota = (int64_t)mincurrent + quota/(int64_t)pageSize +1;

      size_t mintop = memMinHardTop / pageSize;
      size_t maxtop = memMaxHardTop / pageSize;

      if(quota>mintop) 
      {
         quota = mintop;
         rt = -1;
      }

      if(!SetProcessWorkingSetSize(processHandle, (size_t)quota * pageSize, maxtop * pageSize)) rt = -1;

   #else
   //*nix code goes here
   #endif

      return rt;
   }

   const size_t MemPool::memsize = getPoolSize();
   AtomicInt32 MemPool::lock = 0;

   void Gap::reset()
   {
      position = 0;
      size = 0;
   }

   void BufferHeader::reset()
   {
      memset(this, 0, sizeof(BufferHeader));
      pinuse = &linuse;
   }

   BufferHeader* MemPool::GetBH(unsigned int size)
   {
	   //check to see if a bh isn't already available
      BufferHeader *bhtmp = 0;
      unsigned int i;

	   if(size>0)
	   {
		   for(i=0; i<nBH; i++)
		   {
			   if(!*BH[i]->pinuse)
			   {
               bhtmp = BH[i];
               break;
			   }
		   }
	   }
      else return 0;

	   if(nBH==totalBH)
	   {
		   totalBH+=BHstep;
		   BufferHeader **bht;

		   while(!(bht = (BufferHeader**)malloc(sizeof(BufferHeader*)*totalBH)));
	      while(!(bhtmp = (BufferHeader*)malloc(sizeof(BufferHeader)*BHstep)));
		   memcpy(bht, BH, sizeof(BufferHeader*)*nBH);
         for(i=0; i<BHstep; i++)
            bht[nBH+i] = bhtmp +i;

		   free(BH);
		   BH = bht;
	   }

	   bhtmp = BH[nBH];
      bhtmp->reset();
      bhtmp->index = nBH;
      bhtmp->ref = (void*)this;
	   nBH++;

      bhtmp->size = size +4;
      bhtmp->offset = 0;
      bhtmp->pinuse = &bhtmp->linuse;
	   bhtmp->linuse = 1;

      return bhtmp;
   }
		
   void MemPool::ComputeMem()
   {
	   //reset all bh that aren't inuse anymore, compute reserved based on the deepest bh in pool
	   if(nBH)
	   {
		   unsigned int sm=0, g=0;

	      for(g; g<nBH; g++)
		   {
			   if(*BH[g]->pinuse)
			   {
				   if(BH[g]->offset>BH[sm]->offset) sm=g;
			   }
            else BH[g]->reset();
		   }

         if(BH[sm]->offset) reserved = (size_t)BH[sm]->offset -4 -(size_t)pool +BH[sm]->size;
		   else reserved = 0;
		   freemem = total - reserved;
	   }
   }

   void* MemPool::GetPool()
   {
      return (void*)pool;
   }

   void MemPool::Alloc(unsigned int size)
   {
      while(lock.Fetch_Or(1));
      passedQuota = !extendLockedMemQuota(size);
      lock = 0;

	   while(!pool)
		   pool = (byte*)malloc(size);

      if(passedQuota) mlock(pool, size);
				
      total = size;
	   freemem = size;
   }

   void MemPool::Free()
   {
      if(passedQuota)
      {
         munlock(pool, total);
         free(pool);

         while(lock.Fetch_Or(1));
         extendLockedMemQuota((int)total*-1);
         lock = 0;
      }
      else free(pool);

      total=0;
      reserved=0;

      pool = 0;
   }

   void MemPool::ExtendGap()
   {
      gaps = (Gap*)realloc(gaps, sizeof(Gap)*(total_ngaps +BHstep));
      memset(gaps +total_ngaps, 0, sizeof(Gap)*BHstep);

      total_ngaps += BHstep;
   }

   void MemPool::AddGap(BufferHeader *bh)
   {
      while(acquireGap.Fetch_Or(1));
   
      if((size_t)bh->offset - (size_t)pool +bh->size -4 == reserved)
         reserved -= bh->size;
      else
      {
         if(ngaps == total_ngaps) ExtendGap();
      
         gaps[ngaps].position = (int)bh->offset -4;
         gaps[ngaps].size = bh->size;

         ngaps++;
      }

      freemem += bh->size;
      acquireGap = 0;
   }

   int MemPool::GetGap(size_t size)
   {
      int offset = 0;
      while(acquireGap.Fetch_Or(1));

      int i, g=-1;
      Gap lgap(0, total);
   
      for(i=0; i<ngaps; i++)
      {
         if(gaps[i].size>=size)
         {
            lgap = gaps[i];
            g = i;
         }
      }

      if(g>-1)
      {
         offset = lgap.position;
      
         if(size<lgap.size)
         {
            gaps[g].position += size;
            gaps[g].size -= size;
         }
         else
         {
            gaps[g].reset();
            memcpy(gaps +g, gaps +g+1, sizeof(Gap)*(ngaps -g -1));
            ngaps--;
         }
      }
      else 
      {
         offset = (int)pool +reserved;
         reserved += size;
      }

      if(unsigned int(offset -(int)pool +size) > total) 
      {
         offset = 0;
         reserved -= size;
      }
      else freemem -= size;

      acquireGap = 0;
      return offset;
   }
		
   BufferHeader* MemPool::GetBuffer(unsigned int size, unsigned int *sema) //needs fixing
   {
      size+=4; //extra 4 bytes to point at the bh

	   if(lockpool.Fetch_Or(1)) return 0; //pools are meant to function as single threaded
	   if(!total) 
	   {
		   if(size<memsize) Alloc(memsize);
		   else Alloc(size);
	   }
	   else if(size>freemem) 
	   {
		   lockpool = 0;
		   return 0;
	   }

      int offset = GetGap(size);
	   if(!offset)
      {
    	   lockpool = 0;
         return 0;
      }

      BufferHeader *bhtmp = GetBH(size -4);

      if(!bhtmp) 
      {
         lockpool = 0;
         return 0;
      }

      bhtmp->offset = (byte*)offset +4;
      int *bhhead = (int*)offset;
      *bhhead = (int)bhtmp;

	   if(sema) bhtmp->pinuse=sema;
	   bhtmp->move = 0;
	   lockpool = 0;
	   return bhtmp;
   }

   void* CustomAllocator::customAlloc(size_t size)
   {
      //returns mlocked buffer of lenght 'size' (in bytes)
      BufferHeader *bh = GetBuffer(size, 0);
      return (void*)bh->offset;
   }

   void CustomAllocator::customFree(void *buffer)
   {
      //Nulls a buffer previously allocated by customAlloc and marks it as unused
      //frees empty pools
      BufferHeader *bh = (BufferHeader*)*((int*)buffer -1);
      if(bh->linuse==1)
      {
         *(bh->pinuse) = 0;
         memset(bh->offset, 0, bh->size-4);
         MemPool *mp = (MemPool*)bh->ref;
         mp->AddGap(bh);
         bh->offset = 0;

         if(mp->freemem==mp->total)
         {
            CustomAllocator *ca = (CustomAllocator*)mp->ref;
            if(ca) ca->FreePool(mp);
         }
      }
   }

   BufferHeader* CustomAllocator::GetBuffer(unsigned int size, unsigned int *sema)
   {
	   /*** set new lock system here ***/

	   unsigned int i;
	   MemPool *mp;
	   BufferHeader *bh;
			
	   fetchloop:
	   if(clearpool==0) getpoolflag++;
      else goto waithere;
   
      while((i=bufferfetch.Fetch_Add(1))<npools)
	   {
		   mp = MP[order[i]];

		   if(mp->freemem>=size || !mp->total) //look for available size
		   {
	         getpoolflag--;
			   bh = mp->GetBuffer(size, sema);
			   if(bh)
			   {
				   bufferfetch = 0;
				   UpdateOrder(i);
				   return bh;
            }
			   else 
            {
               UpdateOrder(i);
               if(clearpool==0) getpoolflag++;
               else goto waithere;
            }
		   }
	   }
						
	   getpoolflag--;

	   //either all pools are locked or they're full, add a new batch of pools
      while(getpoolflag!=0);


      if(!ab.Fetch_Or(1))
	   {
		   bufferfetch = npools;
		   ExtendPool();
		   bufferfetch = 0;
		   ab = 0;
	   }

      waithere:
	   while(ab!=0);
	   goto fetchloop;
   }

   void CustomAllocator::UpdateOrder(unsigned int in)
   {
	   if(orderlvl.Fetch_Add(1)==poolstep*2)
	   {
		   in++;
		   while(ab!=0); //extendpool lock
		   ordering = 1;
				
		   unsigned int *ordtmp;
		   memcpy(order2, order+in, sizeof(int)*(npools-in));
		   memcpy(order2 +npools -in, order, sizeof(int)*in);
				
		   ordtmp = order;
		   order = order2;
		   order2 = ordtmp;
				
		   orderlvl = 0;
		   ordering = 0;
	   }
   }

   void CustomAllocator::ExtendPool()
   {
	   while(ordering!=0); //updateorder lock

	   unsigned int F, I;
      int T;
	   unsigned int S = npools +poolstep;

      T = S - total;
      if(T>0) 
      {
	      MemPool **mptmp = (MemPool**)malloc(sizeof(MemPool*)*S);
	      memcpy(mptmp, MP, sizeof(MemPool*)*total);

	      unsigned int *ordtmp = order2;
	      order2 = (unsigned int*)malloc(sizeof(int)*S);
	      memcpy(order2 +T, order, sizeof(int)*total);
			
	      MemPool **mptmp2 = MP;
	      MemPool *mptmp3 = new MemPool[T];
      
         poolbatch = (MemPool**)realloc(poolbatch, sizeof(MemPool*)*(nbatch+1));
         poolbatch[nbatch] = mptmp3;
         nbatch++;

	      for(I=total; I<S; I++)
	      {
		      F = I-total;
		      mptmp[I] = &mptmp3[F];
            mptmp[I]->ref = this;
		      order2[F] = I;
	      }

	      orderlvl=0;
   
         MP = mptmp;

	      free(ordtmp);
	      ordtmp = order;
	      order = order2;
			
	      order2 = (unsigned int*)malloc(sizeof(int)*S);
	      free(ordtmp);
	      free(mptmp2);
   
         total = S;
      }

	   npools = S;
   }

   void CustomAllocator::FreePool(MemPool *pool)
   {
      unsigned int i=0;
   
      for(i; i<total; i++)
      {
         if(pool->GetPool()==MP[i]->GetPool())
         {
            while(ab.Fetch_Or(1));

            clearpool = 1;

            while(ordering!=0);
            while(getpoolflag!=0);

            if(pool->freemem==pool->total)
            {
               MP[i]->Free();
         
               MemPool *swap = MP[i];
               MP[i] = MP[npools-1];
               MP[npools-1] = swap;
         
               int iswap = order[i];
               order[i] = order[npools-1];
               order[npools-1] = iswap;

               npools--;
            }

            clearpool = 0;
            ab = 0;

            break;
         }
      }
   }
		
   void CustomAllocator::FillRate()
   {
	   unsigned int i, hd=0, ld=0, tfmem=0;
      //hd: high density
      //ld: low density
	   float c;
   
	   for(i=0; i<npools; i++)
	   {
		   c = (float)MP[i]->freemem/(float)MP[i]->total;
		   if(c<=0.2f) hd++;
		   else if(c>=0.8f) ld++;

         tfmem += MP[i]->freemem;
	   }

	   float fhd = (float)hd/(float)npools;
	   float fld = (float)ld/(float)npools;

	   int abc=0;
   }

   CustomAllocator CAllocStatic::CA;

   void* CAllocStatic::alloc_(size_t size)
      {return CA.customAlloc(size);}
         
   void  CAllocStatic::free_(void* buffer)
      {CA.customFree(buffer);}

}
