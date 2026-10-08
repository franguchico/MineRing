package dev.ermc.bridge.link;

/** No native pointer or native character handle leaves this machine. */
public record NativePeerInfo(long localSteamId, long remoteSteamId, int flags) {
    public static final NativePeerInfo EMPTY = new NativePeerInfo(0, 0, 0);
    public boolean localValid() { return (flags & 1) != 0 && localSteamId != 0; }
    public boolean matchesRemote(long id) { return (flags & 2) != 0 && id != 0 && id == remoteSteamId; }
    public boolean remoteHidden() { return (flags & 4) != 0; }
}
