package dev.ermc.bridge.mixin;

import net.minecraft.server.players.PlayerList;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Mutable;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(PlayerList.class)
public interface CoopPlayerListAccess {
    @Mutable @Accessor("maxPlayers") void erbridge$maxPlayers(int count);
}
