package dev.ermc.bridge;

import it.unimi.dsi.fastutil.longs.Long2ObjectLinkedOpenHashMap;
import it.unimi.dsi.fastutil.longs.Long2ObjectMap;

/** Bounded server-thread metadata only. Eviction never edits blocks or loads a chunk. */
final class TerrainCache<V> {
	private final int capacity;
	private final Long2ObjectLinkedOpenHashMap<V> entries;
	private long evictions;
	private final it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap versions = new it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap();
	private final it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<Revision> committed = new it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<>();
	private final it.unimi.dsi.fastutil.longs.Long2IntOpenHashMap pending = new it.unimi.dsi.fastutil.longs.Long2IntOpenHashMap();
	private long generation = 1, nextVersion;
	/** A queued cast owns this revision even before a physical/cache column exists. */
	record Revision(long generation, long column) { }

	TerrainCache(int capacity) {
		if (capacity < 1) throw new IllegalArgumentException("capacity");
		this.capacity = capacity;
		entries = new Long2ObjectLinkedOpenHashMap<>(capacity);
	}

	V get(long key) { return entries.getAndMoveToLast(key); }
	/** Horizon scans must not displace the actual feet from the LRU. */
	V peek(long key) { return entries.get(key); }
	boolean current(long key) { return entries.containsKey(key) && matches(key,committed.get(key)); }
	Revision capture(long key) {
		if (!versions.containsKey(key)) versions.put(key,++nextVersion);
		pending.addTo(key,1);
		return new Revision(generation,versions.get(key));
	}
	void release(long key) {
		int count=pending.get(key);
		if (count>1) pending.put(key,count-1);
		else { pending.remove(key); if (!entries.containsKey(key)) versions.remove(key); }
	}
	boolean matches(long key, Revision revision) {
		return revision!=null && revision.generation()==generation && versions.containsKey(key) && revision.column()==versions.get(key);
	}
	/** Retain the physical column while its replacement is pending. */
	void invalidate(long key) { if (versions.containsKey(key)) versions.put(key,++nextVersion); }
	void invalidateAll() { generation++; }
	boolean containsKey(long key) { return entries.containsKey(key); }
	void put(long key, V value) {
		retain(key,value);
		if (!versions.containsKey(key)) versions.put(key,++nextVersion);
		committed.put(key,new Revision(generation,versions.get(key)));
	}
	boolean putIfCurrent(long key, V value, Revision revision) {
		if (!matches(key,revision)) return false;
		put(key,value); return true;
	}
	/** Cleanup metadata may retain existing certified collision; it never refreshes its revision. */
	void retain(long key, V value) {
		if (value == null) throw new NullPointerException("value");
		if (!entries.containsKey(key) && entries.size() == capacity) {
			long evicted=entries.firstLongKey();
			committed.remove(evicted);
			if (pending.containsKey(evicted)) versions.put(evicted,++nextVersion);
			else versions.remove(evicted);
			entries.removeFirst();
			evictions++;
		}
		entries.putAndMoveToLast(key, value);
	}
	V remove(long key) {
		committed.remove(key);
		if (pending.containsKey(key)) versions.put(key,++nextVersion); else versions.remove(key);
		return entries.remove(key);
	}
	int size() { return entries.size(); }
	long evictions() { return evictions; }
	Iterable<Long2ObjectMap.Entry<V>> long2ObjectEntrySet() { return entries.long2ObjectEntrySet(); }
	void clear() { entries.clear(); versions.clear(); committed.clear(); pending.clear(); generation++; evictions = 0; }
}
