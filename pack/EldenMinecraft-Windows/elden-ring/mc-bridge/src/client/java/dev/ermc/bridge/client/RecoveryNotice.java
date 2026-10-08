package dev.ermc.bridge.client;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.pipeline.TextureTarget;
import com.mojang.blaze3d.platform.GlStateManager;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vertex.ByteBufferBuilder;
import com.mojang.blaze3d.vertex.VertexSorting;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.GuiGraphics;
import net.minecraft.client.renderer.MultiBufferSource;
import net.minecraft.client.renderer.ShaderInstance;
import org.joml.Matrix4f;
import org.joml.Matrix4fStack;
import org.lwjgl.opengl.GL11;
import org.lwjgl.opengl.GL13;
import org.lwjgl.opengl.GL14;
import org.lwjgl.opengl.GL15;
import org.lwjgl.opengl.GL20;
import org.lwjgl.opengl.GL30;
import org.lwjgl.opengl.GL33;

/** Notice-only producer. Main wires recovery/retry and the passive third-plane readback. */
public final class RecoveryNotice {
    private RecoveryNotice() {}
    private static final RecoveryNoticePolicy POLICY = new RecoveryNoticePolicy();
    private static TextureTarget target;

    private static RecoveryNoticePolicy.Identity identity(Minecraft mc) {
        return new RecoveryNoticePolicy.Identity(mc.getConnection(), mc.level, mc.player);
    }

    public static void show(Minecraft mc, String reason) {
        RenderSystem.assertOnRenderThread();
        if (mc == null) clear();
        else POLICY.show(identity(mc), reason, System.nanoTime());
    }

    public static void clear() { POLICY.clear(); }

    /** Recheck when publishing an asynchronous capture; a draw cannot extend the lifetime. */
    public static int remainingTtlMs(Minecraft mc) {
        return mc == null ? 0 : POLICY.remainingTtlMs(identity(mc), System.nanoTime());
    }

    /** Read this target only after draw/capture returned a positive TTL for this render. */
    public static RenderTarget colorTarget() { return target; }

    public static int capture(Minecraft mc) { return draw(mc); }

    /** Draw only new reason/retry pixels onto a separately cleared transparent color target. */
    public static int draw(Minecraft mc) {
        RenderSystem.assertOnRenderThread();
        if (remainingTtlMs(mc) == 0) return 0;
        RenderTarget main = mc.getMainRenderTarget();
        if (main.width <= 0 || main.height <= 0) return 0;
        GlSnapshot saved = new GlSnapshot();
        Matrix4f projection = new Matrix4f(RenderSystem.getProjectionMatrix());
        VertexSorting sorting = RenderSystem.getVertexSorting();
        Matrix4fStack modelView = RenderSystem.getModelViewStack();
        modelView.pushMatrix();
        try (ByteBufferBuilder bytes = new ByteBufferBuilder(8192)) {
            if (target == null || target.width != main.width || target.height != main.height) {
                if (target != null) target.destroyBuffers();
                target = new TextureTarget(main.width, main.height, false, Minecraft.ON_OSX);
            }
            target.bindWrite(true);
            RenderSystem.disableScissor();
            RenderSystem.colorMask(true, true, true, true);
            RenderSystem.clearColor(0, 0, 0, 0);
            RenderSystem.clear(GL11.GL_COLOR_BUFFER_BIT, Minecraft.ON_OSX);
            int width = Math.max(1, mc.getWindow().getGuiScaledWidth());
            int height = Math.max(1, mc.getWindow().getGuiScaledHeight());
            RenderSystem.setProjectionMatrix(new Matrix4f().setOrtho(0, width, height, 0, 1000, 21000),
                VertexSorting.ORTHOGRAPHIC_Z);
            modelView.identity().translate(0, 0, -11000);
            RenderSystem.applyModelViewMatrix();
            RenderSystem.disableDepthTest();
            RenderSystem.depthMask(false);
            RenderSystem.disableCull();
            RenderSystem.enableBlend();
            // RGB premultiplication comes from SRC_ALPHA onto transparent black;
            // alpha uses ONE, so transport alpha is not accidentally squared.
            RenderSystem.defaultBlendFunc();
            RenderSystem.setShaderColor(1, 1, 1, 1);
            GuiGraphics graphics = new GuiGraphics(mc, MultiBufferSource.immediate(bytes));
            int maxWidth = Math.max(1, width - 24);
            String reason = mc.font.plainSubstrByWidth(POLICY.reason(), maxWidth - Math.min(16, maxWidth - 1));
            String retry = mc.font.plainSubstrByWidth(RecoveryNoticePolicy.RETRY, maxWidth - Math.min(16, maxWidth - 1));
            int bannerWidth = Math.min(maxWidth, Math.max(mc.font.width(reason), mc.font.width(retry)) + 16);
            int x = (width - bannerWidth) / 2, y = Math.min(12, Math.max(0, height - 38));
            // Opaque background also makes all glyph edges premultiplied without
            // changing Minecraft RenderType's own blend functions.
            graphics.fill(x, y, x + bannerWidth, Math.min(height, y + 34), 0xFF182029);
            graphics.drawString(mc.font, reason, x + 8, y + 6, 0xFFFFD685, false);
            graphics.drawString(mc.font, retry, x + 8, y + 19, 0xFFF2F5F7, false);
            graphics.flush();
            return remainingTtlMs(mc);
        } finally {
            modelView.popMatrix();
            RenderSystem.applyModelViewMatrix();
            RenderSystem.setProjectionMatrix(projection, sorting);
            saved.restore();
        }
    }

    /** Restore actual entry state via MC wrappers as well as the few uncached GL bindings. */
    private static final class GlSnapshot {
        final int readFbo = GL11.glGetInteger(GL30.GL_READ_FRAMEBUFFER_BINDING);
        final int drawFbo = GL11.glGetInteger(GL30.GL_DRAW_FRAMEBUFFER_BINDING);
        final int activeTexture = GL11.glGetInteger(GL13.GL_ACTIVE_TEXTURE);
        final int[] viewport = new int[4], scissor = new int[4];
        final float[] clearColor = new float[4], shaderColor = RenderSystem.getShaderColor().clone();
        final byte[] colorMask = new byte[4];
        final boolean depth = GL11.glIsEnabled(GL11.GL_DEPTH_TEST), cull = GL11.glIsEnabled(GL11.GL_CULL_FACE);
        final boolean blend = GL11.glIsEnabled(GL11.GL_BLEND), scissorEnabled = GL11.glIsEnabled(GL11.GL_SCISSOR_TEST);
        final boolean depthMask = GL11.glGetBoolean(GL11.GL_DEPTH_WRITEMASK);
        final int depthFunc = GL11.glGetInteger(GL11.GL_DEPTH_FUNC);
        final int srcRgb = GL11.glGetInteger(GL14.GL_BLEND_SRC_RGB), dstRgb = GL11.glGetInteger(GL14.GL_BLEND_DST_RGB);
        final int srcAlpha = GL11.glGetInteger(GL14.GL_BLEND_SRC_ALPHA), dstAlpha = GL11.glGetInteger(GL14.GL_BLEND_DST_ALPHA);
        final int eqRgb = GL11.glGetInteger(GL20.GL_BLEND_EQUATION_RGB), eqAlpha = GL11.glGetInteger(GL20.GL_BLEND_EQUATION_ALPHA);
        final int program = GL11.glGetInteger(GL20.GL_CURRENT_PROGRAM);
        final int vertexArray = GL11.glGetInteger(GL30.GL_VERTEX_ARRAY_BINDING);
        final int arrayBuffer = GL11.glGetInteger(GL15.GL_ARRAY_BUFFER_BINDING);
        final ShaderInstance shader = RenderSystem.getShader();
        final int[] textures = new int[12], samplers = new int[12], shaderTextures = new int[12];

        GlSnapshot() {
            GL11.glGetIntegerv(GL11.GL_VIEWPORT, viewport);
            GL11.glGetIntegerv(GL11.GL_SCISSOR_BOX, scissor);
            GL11.glGetFloatv(GL11.GL_COLOR_CLEAR_VALUE, clearColor);
            try (org.lwjgl.system.MemoryStack stack = org.lwjgl.system.MemoryStack.stackPush()) {
                var masks = stack.malloc(4);
                GL11.glGetBooleanv(GL11.GL_COLOR_WRITEMASK, masks);
                masks.get(colorMask);
            }
            for (int unit = 0; unit < textures.length; unit++) {
                RenderSystem.activeTexture(GL13.GL_TEXTURE0 + unit);
                textures[unit] = GL11.glGetInteger(GL11.GL_TEXTURE_BINDING_2D);
                samplers[unit] = GL30.glGetIntegeri(GL33.GL_SAMPLER_BINDING, unit);
                shaderTextures[unit] = RenderSystem.getShaderTexture(unit);
            }
            RenderSystem.activeTexture(activeTexture);
        }

        void restore() {
            GlStateManager._glBindFramebuffer(GL30.GL_READ_FRAMEBUFFER, readFbo);
            GlStateManager._glBindFramebuffer(GL30.GL_DRAW_FRAMEBUFFER, drawFbo);
            RenderSystem.viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            GlStateManager._scissorBox(scissor[0], scissor[1], scissor[2], scissor[3]);
            if (scissorEnabled) GlStateManager._enableScissorTest(); else RenderSystem.disableScissor();
            if (depth) RenderSystem.enableDepthTest(); else RenderSystem.disableDepthTest();
            RenderSystem.depthFunc(depthFunc);
            RenderSystem.depthMask(depthMask);
            if (cull) RenderSystem.enableCull(); else RenderSystem.disableCull();
            RenderSystem.blendFuncSeparate(srcRgb, dstRgb, srcAlpha, dstAlpha);
            GL20.glBlendEquationSeparate(eqRgb, eqAlpha);
            if (blend) RenderSystem.enableBlend(); else RenderSystem.disableBlend();
            RenderSystem.colorMask(colorMask[0] != 0, colorMask[1] != 0, colorMask[2] != 0, colorMask[3] != 0);
            RenderSystem.clearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
            RenderSystem.setShaderColor(shaderColor[0], shaderColor[1], shaderColor[2], shaderColor[3]);
            RenderSystem.setShader(() -> shader);
            GlStateManager._glUseProgram(program);
            com.mojang.blaze3d.vertex.BufferUploader.invalidate();
            GlStateManager._glBindVertexArray(vertexArray);
            GlStateManager._glBindBuffer(GL15.GL_ARRAY_BUFFER, arrayBuffer);
            for (int unit = 0; unit < textures.length; unit++) {
                RenderSystem.setShaderTexture(unit, shaderTextures[unit]);
                RenderSystem.activeTexture(GL13.GL_TEXTURE0 + unit);
                GlStateManager._bindTexture(textures[unit]);
                GL33.glBindSampler(unit, samplers[unit]);
            }
            RenderSystem.activeTexture(activeTexture);
        }
    }
}
