package dev.ermc.bridge.mixin;

import dev.ermc.bridge.coop.CoopBodyGuard;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Client-side suspension alone cannot stop the server simulating an inactive MC body. */
@Mixin(Player.class)
public abstract class CoopServerPlayerPhysicsMixin {
    @Inject(method = "travel(Lnet/minecraft/world/phys/Vec3;)V", at = @At("HEAD"),
        cancellable = true, require = 1, allow = 1)
    private void erbridge$suspendServerBody(Vec3 input, CallbackInfo ci) {
        if (!((Object) this instanceof ServerPlayer player) || !CoopBodyGuard.isSuspended(player)) return;
        CoopBodyGuard.stopMotion(player);
        ci.cancel();
    }
}
