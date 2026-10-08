package dev.ermc.bridge.entity.combat;

import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.UUID;

/** Server-thread counter baselines; connection/life epochs come from the authenticated session. */
public final class PlayerEventLedger<E> {
	public record Delta(boolean hostDeath, boolean hit, float damage, float status) {
		public static final Delta NONE = new Delta(false, false, 0, 0);
	}

	private static final class State<E> {
		final E epoch;
		long sequence = -1;
		boolean initialized;
		int deaths;
		long hits;
		float damage, status;
		double pendingStatus;
		long statusTick;

		State(E epoch) { this.epoch = epoch; }
	}

	private final Map<UUID, State<E>> players = new HashMap<>();

	/** Bind only the session's current epoch, never an epoch taken from an unchecked packet. */
	public void bind(UUID player, E epoch) {
		Objects.requireNonNull(player);
		Objects.requireNonNull(epoch);
		State<E> state = players.get(player);
		if (state == null || !state.epoch.equals(epoch)) players.put(player, new State<>(epoch));
	}

	public void forget(UUID player) { players.remove(player); }
	public void retain(Set<UUID> connected) { players.keySet().retainAll(connected); }
	public void clear() { players.clear(); }
	public int size() { return players.size(); }

	/**
	 * Consume a complete sample once. A first sample, reset or new epoch establishes
	 * a baseline instead of catching up historical damage. Repeated samples may
	 * drain already accepted status damage at 20 server ticks, but cannot add it twice.
	 */
	public Delta observe(UUID player, E epoch, long sequence, int deaths, long hits,
			float damage, float status, long tick) {
		State<E> state = players.get(player);
		if (state == null || !state.epoch.equals(epoch) || sequence < 0 || sequence < state.sequence) return Delta.NONE;
		if (sequence == state.sequence) return drain(state, false, false, 0, tick);
		state.sequence = sequence;
		if (hits < 0 || !Float.isFinite(damage) || damage < 0 || !Float.isFinite(status) || status < 0) {
			state.initialized = false;
			state.pendingStatus = 0;
			return Delta.NONE;
		}
		if (!state.initialized || Integer.compareUnsigned(deaths, state.deaths) < 0 ||
			hits < state.hits || damage < state.damage || status < state.status) {
			state.initialized = true;
			state.deaths = deaths;
			state.hits = hits;
			state.damage = damage;
			state.status = status;
			state.pendingStatus = 0;
			state.statusTick = tick;
			return Delta.NONE;
		}
		boolean died = deaths != state.deaths;
		float delta = hits > state.hits ? damage - state.damage : 0;
		state.pendingStatus += status - state.status;
		state.deaths = deaths;
		state.hits = hits;
		state.damage = damage;
		state.status = status;
		return drain(state, died, delta > 0, delta, tick);
	}

	private Delta drain(State<E> state, boolean died, boolean hit, float damage, long tick) {
		float status = 0;
		if (state.initialized && tick - state.statusTick >= 20) {
			status = (float) Math.min(Float.MAX_VALUE, state.pendingStatus);
			state.pendingStatus = 0;
			state.statusTick = tick;
		}
		return died || hit || status > 0 ? new Delta(died, hit, damage, status) : Delta.NONE;
	}
}
