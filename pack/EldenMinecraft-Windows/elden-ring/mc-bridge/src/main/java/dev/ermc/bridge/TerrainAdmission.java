package dev.ermc.bridge;

import java.util.HashMap;
import java.util.Set;

/** Server-thread ingress latch. Local movement gaps cannot re-open a completed admission. */
final class TerrainAdmission<K,V> {
	private final HashMap<K,V> waiting = new HashMap<>();
	private final HashMap<K,TerrainReadiness> evidence = new HashMap<>();
	private final HashMap<K,TerrainReadiness> current = new HashMap<>();
	void prime(K key, V target) { waiting.put(key,target); evidence.put(key,TerrainReadiness.PENDING); current.put(key,TerrainReadiness.PENDING); }
	V waiting(K key) { return waiting.get(key); }
	boolean ready(K key) {
		TerrainReadiness certified=evidence.get(key);
		return !waiting.containsKey(key) && certified!=null && certified.permitsMovement();
	}
	TerrainReadiness result(K key) { return evidence.getOrDefault(key,TerrainReadiness.PENDING); }
	TerrainReadiness current(K key) { return current.getOrDefault(key,TerrainReadiness.PENDING); }
	void observe(K key, TerrainReadiness result) {
		current.put(key,result);
		// Main uses this evidence for admission status 8. A jump, real void step or missing
		// neighbour after a certified admission must not masquerade as a failed recall.
		TerrainReadiness admitted=evidence.get(key);
		if (!waiting.containsKey(key) && admitted!=null && admitted.permitsMovement()) return;
		evidence.put(key,result);
		if (result.permitsMovement()) waiting.remove(key);
	}
	void remove(K key) { waiting.remove(key); evidence.remove(key); current.remove(key); }
	void retain(Set<K> keys) { waiting.keySet().retainAll(keys); evidence.keySet().retainAll(keys); current.keySet().retainAll(keys); }
	void clear() { waiting.clear(); evidence.clear(); current.clear(); }
}
