package dev.ermc.bridge.link;

import java.nio.file.Path;
import java.util.Locale;
import java.util.Map;

/** Shared IPC directory contract with the native bridge. No Minecraft or LWJGL dependency. */
public final class BridgePaths {
	private BridgePaths() {
	}

	private static final class DefaultDirectory {
		private static final Path VALUE = resolveDirectory(System.getProperty("os.name"), System.getenv());
	}

	public static Path directory() {
		return DefaultDirectory.VALUE;
	}

	public static Path bridgeShm() {
		return directory().resolve("bridge.shm");
	}

	public static Path framesShm() {
		return directory().resolve("frames.shm");
	}

	public static Path commandsFile() {
		return directory().resolve("mc_cmd.txt");
	}

	/** Package-visible so path validation can be tested without changing the process environment. */
	static Path resolveDirectory(String osName, Map<String, String> environment) {
		String override = environment.get("ERMC_DIR");
		if (override != null && !override.isEmpty()) {
			return absoluteDirectory("ERMC_DIR", override);
		}
		if (osName.toLowerCase(Locale.ROOT).startsWith("windows")) {
			String userProfile = environment.get("USERPROFILE");
			if (userProfile == null || userProfile.isEmpty()) {
				throw new IllegalArgumentException("Set an absolute ERMC_DIR or USERPROFILE on Windows");
			}
			// Packaged launchers can virtualize LOCALAPPDATA into a different physical file.
			return absoluteDirectory("USERPROFILE", userProfile).resolve("Documents").resolve("EldenMinecraft").resolve("ipc");
		}
		// Preserve the original macOS/CrossOver directory (and the existing Unix behavior).
		return Path.of("/tmp/ermc");
	}

	private static Path absoluteDirectory(String variable, String value) {
		Path directory = Path.of(value);
		if (!directory.isAbsolute()) {
			throw new IllegalArgumentException(variable + " must be an absolute directory: " + value);
		}
		return directory.normalize();
	}
}
