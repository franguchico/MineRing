package dev.ermc.bridge.mixin;

import com.mojang.authlib.GameProfile;
import dev.ermc.bridge.coop.CoopSession;
import java.net.SocketAddress;
import net.minecraft.network.chat.Component;
import net.minecraft.server.players.PlayerList;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(PlayerList.class)
public abstract class CoopAdmissionMixin {
    // IntegratedPlayerList.isWhiteListed normally returns true. Check the actual UUID list
    // before vanilla's virtual call, independently of optional e4mc command mixins.
    @Inject(method = "canPlayerLogin", at = @At("HEAD"), cancellable = true)
    private void erbridge$admission(SocketAddress address, GameProfile profile, CallbackInfoReturnable<Component> ci) {
        Component reason = CoopSession.admission(((PlayerList) (Object) this).getServer(), profile);
        if (reason != null) ci.setReturnValue(reason);
    }
    @Inject(method = "isOp", at = @At("HEAD"), cancellable = true)
    private void erbridge$guestPermissions(GameProfile profile, CallbackInfoReturnable<Boolean> ci) {
        var server = ((PlayerList) (Object) this).getServer();
        if (CoopSession.enabled() && CoopSession.isHostAuthority(server) && !CoopSession.ownerProfile(server, profile))
            ci.setReturnValue(false);
    }
}
