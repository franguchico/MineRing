package dev.ermc.bridge.client.mixin;

import dev.ermc.bridge.client.Overlay;
import net.minecraft.client.gui.Gui;
import net.minecraft.client.gui.GuiGraphics;
import net.minecraft.client.DeltaTracker;
import net.minecraft.world.entity.Entity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** The vignette is drawn with a blend mode that would make the whole window opaque. */
@Mixin(Gui.class)
public abstract class GuiMixin {
	@Inject(method = "render", at = @At("HEAD"), cancellable = true, require = 1, allow = 1)
	private void erbridge$passiveNoHud(GuiGraphics graphics, DeltaTracker delta, CallbackInfo ci) {
		if (Overlay.passive()) ci.cancel();
	}
	@Inject(method = "renderVignette", at = @At("HEAD"), cancellable = true)
	private void erbridge$noVignette(GuiGraphics graphics, Entity entity, CallbackInfo ci) {
		if (Overlay.active()) {
			ci.cancel();
		}
	}
}
