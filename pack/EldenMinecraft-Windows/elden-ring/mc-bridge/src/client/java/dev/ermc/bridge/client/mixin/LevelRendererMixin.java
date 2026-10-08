package dev.ermc.bridge.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.VertexConsumer;
import dev.ermc.bridge.ErBridgeMod;
import dev.ermc.bridge.client.Overlay;
import net.minecraft.client.Camera;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.LightTexture;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.block.state.BlockState;
import org.joml.Matrix4f;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * The host game supplies the sky, clouds and weather; Minecraft's would paint over them. The
 * synthetic terrain has collision and a placement target, but no visible selection outline.
 * Ordinary Minecraft blocks keep their vanilla outline.
 */
@Mixin(LevelRenderer.class)
public abstract class LevelRendererMixin {
	@Inject(method = "renderHitOutline", at = @At("HEAD"), cancellable = true, require = 1, allow = 1)
	private void erbridge$noTerrainOutline(PoseStack poseStack, VertexConsumer vertices, Entity entity,
										 double camX, double camY, double camZ, BlockPos pos, BlockState state,
										 CallbackInfo ci) {
		// Hide only the synthetic voxel edges; keep the hit result and shapes for placement/collision.
		if (state.is(ErBridgeMod.TERRAIN)) {
			ci.cancel();
		}
	}

	@Inject(method = "renderSky", at = @At("HEAD"), cancellable = true)
	private void erbridge$noSky(Matrix4f modelView, Matrix4f projection, float partialTick, Camera camera,
								 boolean foggy, Runnable setupFog, CallbackInfo ci) {
		if (Overlay.active()) {
			ci.cancel();
		}
	}

	@Inject(method = "renderClouds", at = @At("HEAD"), cancellable = true)
	private void erbridge$noClouds(PoseStack poseStack, Matrix4f modelView, Matrix4f projection, float partialTick,
									double camX, double camY, double camZ, CallbackInfo ci) {
		if (Overlay.active()) {
			ci.cancel();
		}
	}

	@Inject(method = "renderSnowAndRain", at = @At("HEAD"), cancellable = true)
	private void erbridge$noWeather(LightTexture lightTexture, float partialTick, double camX, double camY, double camZ,
									 CallbackInfo ci) {
		if (Overlay.active()) {
			ci.cancel();
		}
	}
}
