package dev.ermc.bridge;

import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.chunk.LevelChunk;

/** Removes only bridge collision blocks from an already-loaded column. */
final class TerrainCleanup {
	private TerrainCleanup() { }
	/** One budgeted probe/mutation. Returns a cursor beyond an entirely empty section. */
	static int step(ServerLevel level, LevelChunk chunk, int x, int y, int z) {
		var section = chunk.getSection(level.getSectionIndex(y));
		if (section.hasOnlyAir()) return (y | 15) + 1;
		// Avoid scanning a section's palette for every column: one exact state read is bounded.
		if (section.getBlockState(x & 15, y & 15, z & 15).is(ErBridgeMod.TERRAIN)) {
			level.setBlock(new BlockPos(x,y,z), Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
		}
		return y + 1;
	}
}
