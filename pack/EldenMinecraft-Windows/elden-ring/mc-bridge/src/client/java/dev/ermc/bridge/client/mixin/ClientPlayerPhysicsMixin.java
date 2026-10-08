package dev.ermc.bridge.client.mixin;

import dev.ermc.bridge.client.CoopModeClient;
import dev.ermc.bridge.client.Overlay;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Only the local inactive MC body: keep old elytra/gravity from simulating independent travel. */
@Mixin(Player.class)
public abstract class ClientPlayerPhysicsMixin {
    @Inject(method = "travel(Lnet/minecraft/world/phys/Vec3;)V", at = @At("HEAD"),
        cancellable = true, require = 1, allow = 1)
    private void erbridge$passiveBody(Vec3 input, CallbackInfo ci) {
        Player player = (Player) (Object) this;
        if (!CoopModeClient.freezeMinecraftBody(player)) return;
        player.setDeltaMovement(Vec3.ZERO);
        player.fallDistance = 0;
        if (Overlay.hostMode() && player.isFallFlying()) player.stopFallFlying();
        // Server teleports/recalls still apply normally; do not write position or native controls.
        ci.cancel();
    }
}
