package dev.ermc.bridge.client;

import dev.ermc.bridge.CoordMap;
import dev.ermc.bridge.TerrainManager;
import dev.ermc.bridge.coop.CoopPayloads;
import dev.ermc.bridge.coop.CoopSession;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.AbstractClientPlayer;
import net.minecraft.client.renderer.LightTexture;
import net.minecraft.core.BlockPos;
import net.minecraft.core.SectionPos;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.Level;

/** Prevents an unlit player frame while a bridge chunk's light packet is still pending. */
public final class BridgePlayerLighting {
	private BridgePlayerLighting() {
	}

	public static int packedLight(Entity entity, float partialTick, int vanillaLight) {
		if (vanillaLight != 0 || !(entity instanceof AbstractClientPlayer)) {
			return vanillaLight;
		}
		Minecraft mc = Minecraft.getInstance();
		if (mc.level == null || entity.level() != mc.level) {
			return vanillaLight;
		}
		var server = mc.getSingleplayerServer();
		boolean localBridgeWorld = server != null
			&& TerrainManager.BRIDGE_LEVEL_NAME.equals(server.getWorldData().getLevelName());
		// Guests have no TerrainManager server state. A shared, validated anchor comes only
		// from the bridge authority's ACK and is cleared by CoopClient on disconnect.
		boolean guest = server == null && CoopSession.guest();
		boolean bridgeChannel = guest && mc.getConnection() != null
			&& mc.getConnection().getConnection().isConnected()
			&& ClientPlayNetworking.canSend(CoopPayloads.Snapshot.TYPE);
		boolean bridgeWorld = isBridgeWorld(mc.level.dimension() == Level.OVERWORLD, server != null,
			localBridgeWorld, guest, bridgeChannel, guest && CoordMap.validShared(CoordMap.get()));
		if (!bridgeWorld) {
			return vanillaLight;
		}
		// Use exactly the interpolated body probe used by EntityRenderer, not the local
		// player's feet. ClientPacketListener enables this column after applying its light data.
		BlockPos probe = BlockPos.containing(entity.getLightProbePosition(partialTick));
		boolean lightReady = mc.level.getChunkSource().getLightEngine().lightOnInSection(SectionPos.of(probe));
		return resolvePackedLight(vanillaLight, true, true, lightReady);
	}

	/** A co-op launch flag or a stale anchor alone must never brighten a normal world. */
	public static boolean isBridgeWorld(boolean overworld, boolean localServerPresent, boolean localBridgeWorld,
									 boolean coopGuest, boolean bridgeChannel, boolean sharedBridgeMapping) {
		return overworld && (localServerPresent ? localBridgeWorld
			: coopGuest && bridgeChannel && sharedBridgeMapping);
	}

	public static int resolvePackedLight(int vanillaLight, boolean playerInCurrentLevel, boolean bridgeWorld,
										 boolean lightReady) {
		// This is a loading fallback, not permanent fullbright: even a fully dark, ready
		// chunk retains vanilla lighting. The base skin, armor and held items share this
		// value; their textures, model shading, hurt overlay, alpha and depth stay intact.
		return playerInCurrentLevel && bridgeWorld && !lightReady && vanillaLight == 0
			? LightTexture.FULL_BRIGHT : vanillaLight;
	}
}
