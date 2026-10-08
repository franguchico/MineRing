package dev.ermc.bridge.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import dev.ermc.bridge.client.CoopModeClient;
import net.minecraft.client.renderer.MultiBufferSource;
import net.minecraft.client.renderer.entity.EntityRenderDispatcher;
import net.minecraft.world.entity.Entity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Hide the whole peer avatar, including skin layers, equipment, name and shadow. */
@Mixin(EntityRenderDispatcher.class)
public abstract class PeerVisibilityMixin {
    @Inject(method = "render(Lnet/minecraft/world/entity/Entity;DDDFFLcom/mojang/blaze3d/vertex/PoseStack;Lnet/minecraft/client/renderer/MultiBufferSource;I)V",
        at = @At("HEAD"), cancellable = true, require = 1, allow = 1)
    private void erbridge$peerMode(Entity entity, double x, double y, double z, float yaw, float partialTick,
            PoseStack pose, MultiBufferSource buffers, int light, CallbackInfo ci) {
        if (CoopModeClient.hideMinecraft(entity)) ci.cancel();
    }
}
