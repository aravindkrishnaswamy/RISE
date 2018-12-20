//////////////////////////////////////////////////////////////////////
//
//  MRUCache.h - A most-recently used cache template
//
//  Author: Dan McCormick
//  Date of Birth: May 19, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MRUCACHE_
#define MRUCACHE_

#include <list>
#include <algorithm>

namespace RISE
{
	///////////////////////////////////////////////////////////////////////
	// The ValueGenerator is what takes a key type and generates an actual
	// cache item
	///////////////////////////////////////////////////////////////////////
	template<typename Key, typename	Value> 
	struct ValueGenerator
	{
		virtual ~ValueGenerator(){}
		virtual Value * Get( const Key& k ) = 0;
		virtual void Return( const Key& k, Value * v ) = 0;
	};

	///////////////////////////////////////////////////////////////////////
	// The MRUCache
	///////////////////////////////////////////////////////////////////////
	template<typename Key, typename Value> 
	class MRUCache : 
		public ValueGenerator<Key, Value>
	{
		typedef std::pair<Key, Value *> Item;
		static inline bool ItemCompare( const Item& lhs, const Item& rhs )
		{
			return lhs.first < rhs.first;
		}

		struct KeyFind
		{
			const Key& k;
			KeyFind(const Key& k_) : k(k_){}

			inline bool operator()( const Item& rhs ) const
			{
				return rhs.first == k;
			}
		};

		typedef std::list<Item> ItemList;
		ItemList mru;

		ValueGenerator<Key, Value>& gen;
		size_t item_count;

		inline Value * CacheGetAndRemove( const Key& k )
		{
			typename ItemList::iterator it = std::find_if( mru.begin(), mru.end(), KeyFind(k) );
			if( it == mru.end() ) {
				return 0;
			}

			Value * ret = it->second;
			mru.erase(it);
			return ret;
		}

		inline void CacheInsert( const Key& k, Value * v )
		{
			mru.push_front(Item(k, v));
		}

		inline void ControlSize()
		{
			const size_t mru_size = mru.size();
			if(mru_size > item_count)
			{
				size_t to_remove = mru_size - item_count;
				for( ;to_remove > 0; --to_remove )
				{
					Item& item = mru.back();
					gen.Return(item.first, item.second);
					mru.pop_back();
				}
			}
		}

	public:
		MRUCache(ValueGenerator<Key, Value>& gen_, size_t item_count_) :
		gen(gen_), item_count(item_count_)
		{
		}

		virtual ~MRUCache()
		{
			for( typename ItemList::iterator it = mru.begin(); it != mru.end(); ++it ) {
				gen.Return(it->first, it->second);
				it->second = 0;
			}
			mru.clear();
		}

		virtual Value * Get(const Key& k)
		{
			Value * ret = CacheGetAndRemove(k);
			if(!ret) {
				ret = gen.Get(k);
			}
			return ret;
		}

		virtual void Return(const Key& k, Value * v)
		{
			if(!v) {
				return;
			}

			Value * temp = CacheGetAndRemove(k);
			if(temp && temp != v) {
				gen.Return(k, v);
			}

			CacheInsert(k, v);
			ControlSize();
		}
	};
}

#endif //MRUCACHE_
