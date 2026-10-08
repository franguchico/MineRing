package dev.ermc.bridge.client.mixin;

import dev.ermc.bridge.client.BridgePlayerLighting;
import net.minecraft.client.renderer.entity.EntityRenderer;
import net.minecraft.world.entity.Entity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Correct the light shared by the player model and its layers, without changing world light. */
@Mixin(EntityRenderer.class)
public abstract class EntityRendererMixin {
	@Inject(method = "getPackedLightCoords(Lnet/minecraft/world/entity/Entity;F)I",
		at = @At("RETURN"), cancellable = true, require = 1, allow = 1)
	private void erbridge$playerLight(Entity entity, float partialTick, CallbackInfoReturnable<Integer> cir) {
		cir.setReturnValue(BridgePlayerLighting.packedLight(entity, partialTick, cir.getReturnValueI()));
	}
}
